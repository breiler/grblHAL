/*
  planner.c - buffers movement commands and manages the acceleration profile plan
  Part of Grbl

  Copyright (c) 2017-2018 Terje Io
  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011 Jens Geisler

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "grbl.h"

static plan_block_t block_buffer[BLOCK_BUFFER_SIZE];    // A ring buffer for motion instructions
static uint_fast8_t block_buffer_tail;                  // Index of the block to process now
static uint_fast8_t block_buffer_head;                  // Index of the next block to be pushed
static uint_fast8_t next_buffer_head;                   // Index of the next buffer head
static uint_fast8_t block_buffer_planned;               // Index of the optimally planned block

// Define planner variables
typedef struct {
  int32_t position[N_AXIS];         // The planner position of the tool in absolute steps. Kept separate
                                    // from g-code position for movements requiring multiple line motions,
                                    // i.e. arcs, canned cycles, and backlash compensation.
  float previous_unit_vec[N_AXIS];  // Unit vector of previous path line segment
  float previous_nominal_speed;     // Nominal speed of previous path line segment
} planner_t;

static planner_t pl;


// Returns the index of the next block in the ring buffer. Also called by stepper segment buffer.
inline static uint_fast8_t plan_next_block_index (uint_fast8_t block_index)
{
    return block_index == (BLOCK_BUFFER_SIZE - 1) ? 0 : block_index + 1;
}


// Returns the index of the previous block in the ring buffer
inline static uint_fast8_t plan_prev_block_index (uint_fast8_t block_index)
{
    return block_index == 0 ? (BLOCK_BUFFER_SIZE - 1) : block_index - 1;
}


/*                            PLANNER SPEED DEFINITION
                                     +--------+   <- current->nominal_speed
                                    /          \
         current->entry_speed ->   +            \
                                   |             + <- next->entry_speed (aka exit speed)
                                   +-------------+
                                       time -->

  Recalculates the motion plan according to the following basic guidelines:

    1. Go over every feasible block sequentially in reverse order and calculate the junction speeds
        (i.e. current->entry_speed) such that:
      a. No junction speed exceeds the pre-computed maximum junction speed limit or nominal speeds of
         neighboring blocks.
      b. A block entry speed cannot exceed one reverse-computed from its exit speed (next->entry_speed)
         with a maximum allowable deceleration over the block travel distance.
      c. The last (or newest appended) block is planned from a complete stop (an exit speed of zero).
    2. Go over every block in chronological (forward) order and dial down junction speed values if
      a. The exit speed exceeds the one forward-computed from its entry speed with the maximum allowable
         acceleration over the block travel distance.

  When these stages are complete, the planner will have maximized the velocity profiles throughout the all
  of the planner blocks, where every block is operating at its maximum allowable acceleration limits. In
  other words, for all of the blocks in the planner, the plan is optimal and no further speed improvements
  are possible. If a new block is added to the buffer, the plan is recomputed according to the said
  guidelines for a new optimal plan.

  To increase computational efficiency of these guidelines, a set of planner block pointers have been
  created to indicate stop-compute points for when the planner guidelines cannot logically make any further
  changes or improvements to the plan when in normal operation and new blocks are streamed and added to the
  planner buffer. For example, if a subset of sequential blocks in the planner have been planned and are
  bracketed by junction velocities at their maximums (or by the first planner block as well), no new block
  added to the planner buffer will alter the velocity profiles within them. So we no longer have to compute
  them. Or, if a set of sequential blocks from the first block in the planner (or a optimal stop-compute
  point) are all accelerating, they are all optimal and can not be altered by a new block added to the
  planner buffer, as this will only further increase the plan speed to chronological blocks until a maximum
  junction velocity is reached. However, if the operational conditions of the plan changes from infrequently
  used feed holds or feedrate overrides, the stop-compute pointers will be reset and the entire plan is
  recomputed as stated in the general guidelines.

  Planner buffer index mapping:
  - block_buffer_tail: Points to the beginning of the planner buffer. First to be executed or being executed.
  - block_buffer_head: Points to the buffer block after the last block in the buffer. Used to indicate whether
      the buffer is full or empty. As described for standard ring buffers, this block is always empty.
  - next_buffer_head: Points to next planner buffer block after the buffer head block. When equal to the
      buffer tail, this indicates the buffer is full.
  - block_buffer_planned: Points to the first buffer block after the last optimally planned block for normal
      streaming operating conditions. Use for planning optimizations by avoiding recomputing parts of the
      planner buffer that don't change with the addition of a new block, as describe above. In addition,
      this block can never be less than block_buffer_tail and will always be pushed forward and maintain
      this requirement when encountered by the plan_discard_current_block() routine during a cycle.

  NOTE: Since the planner only computes on what's in the planner buffer, some motions with lots of short
  line segments, like G2/3 arcs or complex curves, may seem to move slow. This is because there simply isn't
  enough combined distance traveled in the entire buffer to accelerate up to the nominal speed and then
  decelerate to a complete stop at the end of the buffer, as stated by the guidelines. If this happens and
  becomes an annoyance, there are a few simple solutions: (1) Maximize the machine acceleration. The planner
  will be able to compute higher velocity profiles within the same combined distance. (2) Maximize line
  motion(s) distance per block to a desired tolerance. The more combined distance the planner has to use,
  the faster it can go. (3) Maximize the planner buffer size. This also will increase the combined distance
  for the planner to compute over. It also increases the number of computations the planner has to perform
  to compute an optimal plan, so select carefully. ARM versions should have enough memory and speed for
  look-ahead blocks numbering up to a hundred or more.

*/
static void planner_recalculate ()
{
    // Initialize block index to the last block in the planner buffer.
    uint_fast8_t block_index = plan_prev_block_index(block_buffer_head);

    // Bail. Can't do anything with one only one plan-able block.
    if (block_index == block_buffer_planned)
        return;

    // Reverse Pass: Coarsely maximize all possible deceleration curves back-planning from the last
    // block in buffer. Cease planning when the last optimal planned or tail pointer is reached.
    // NOTE: Forward pass will later refine and correct the reverse pass to create an optimal plan.
    float entry_speed_sqr;
    plan_block_t *next;
    plan_block_t *current = &block_buffer[block_index];

    // Calculate maximum entry speed for last block in buffer, where the exit speed is always zero.
    current->entry_speed_sqr = min(current->max_entry_speed_sqr, 2.0f * current->acceleration * current->millimeters);

    block_index = plan_prev_block_index(block_index);
    if (block_index == block_buffer_planned) { // Only two plannable blocks in buffer. Reverse pass complete.
        // Check if the first block is the tail. If so, notify stepper to update its current parameters.
        if (block_index == block_buffer_tail)
            st_update_plan_block_parameters();
    } else while (block_index != block_buffer_planned) { // Three or more plan-able blocks

        next = current;
        current = &block_buffer[block_index];
        block_index = plan_prev_block_index(block_index);

        // Check if next block is the tail block(=planned block). If so, update current stepper parameters.
        if (block_index == block_buffer_tail)
            st_update_plan_block_parameters();

        // Compute maximum entry speed decelerating over the current block from its exit speed.
        if (current->entry_speed_sqr != current->max_entry_speed_sqr) {
            entry_speed_sqr = next->entry_speed_sqr + 2.0f * current->acceleration * current->millimeters;
            current->entry_speed_sqr = entry_speed_sqr < current->max_entry_speed_sqr ? entry_speed_sqr : current->max_entry_speed_sqr;
        }
    }

    // Forward Pass: Forward plan the acceleration curve from the planned pointer onward.
    // Also scans for optimal plan breakpoints and appropriately updates the planned pointer.
    next = &block_buffer[block_buffer_planned]; // Begin at buffer planned pointer
    block_index = plan_next_block_index(block_buffer_planned);

    while (block_index != block_buffer_head) {

        current = next;
        next = &block_buffer[block_index];

        // Any acceleration detected in the forward pass automatically moves the optimal planned
        // pointer forward, since everything before this is all optimal. In other words, nothing
        // can improve the plan from the buffer tail to the planned pointer by logic.
        if (current->entry_speed_sqr < next->entry_speed_sqr) {
            entry_speed_sqr = current->entry_speed_sqr + 2.0f * current->acceleration * current->millimeters;
        // If true, current block is full-acceleration and we can move the planned pointer forward.
            if (entry_speed_sqr < next->entry_speed_sqr) {
                next->entry_speed_sqr = entry_speed_sqr; // Always <= max_entry_speed_sqr. Backward pass sets this.
                block_buffer_planned = block_index; // Set optimal plan pointer.
            }
        }

        // Any block set at its maximum entry speed also creates an optimal plan up to this
        // point in the buffer. When the plan is bracketed by either the beginning of the
        // buffer and a maximum entry speed or two maximum entry speeds, every block in between
        // cannot logically be further improved. Hence, we don't have to recompute them anymore.
        if (next->entry_speed_sqr == next->max_entry_speed_sqr)
            block_buffer_planned = block_index;

        block_index = plan_next_block_index(block_index);
    }
}


inline static void plan_reset_buffer (bool soft_reset)
{
    if((block_buffer_tail = soft_reset ? BLOCK_BUFFER_SIZE : 0))
      do { // Free memory for any pending messages after soft reset
        if(block_buffer[--block_buffer_tail].message) {
            free(block_buffer[block_buffer_tail].message);
            block_buffer[block_buffer_tail].message = NULL;
        }
    } while(block_buffer_tail);
    block_buffer_head = 0;      // Empty = tail
    next_buffer_head = 1;       // plan_next_block_index(block_buffer_head)
    block_buffer_planned = 0;   // = block_buffer_tail;
}


void plan_reset ()
{
    static bool soft_reset = false;
    memset(&pl, 0, sizeof(planner_t)); // Clear planner struct
    plan_reset_buffer(soft_reset);
    soft_reset = true;
}


void plan_discard_current_block ()
{
    if (block_buffer_head != block_buffer_tail) { // Discard non-empty buffer.
        uint_fast8_t block_index = plan_next_block_index(block_buffer_tail);
        if(block_buffer[block_buffer_tail].message) {
            free(block_buffer[block_buffer_tail].message);
            block_buffer[block_buffer_tail].message = NULL;
        }
        // Push block_buffer_planned pointer, if encountered.
        if (block_buffer_tail == block_buffer_planned)
            block_buffer_planned = block_index;
        block_buffer_tail = block_index;
    }
}


// Returns address of planner buffer block used by system motions. Called by segment generator.
plan_block_t *plan_get_system_motion_block ()
{
    return &block_buffer[block_buffer_head];
}


// Returns address of first planner block, if available. Called by various main program functions.
plan_block_t *plan_get_current_block ()
{
    return block_buffer_head == block_buffer_tail ? NULL : &block_buffer[block_buffer_tail];
}


inline float plan_get_exec_block_exit_speed_sqr ()
{
    uint_fast8_t block_index = plan_next_block_index(block_buffer_tail);
    return block_index == block_buffer_head ? 0.0f : block_buffer[block_index].entry_speed_sqr;
}


// Returns the availability status of the block ring buffer. True, if full.
bool plan_check_full_buffer ()
{
    return block_buffer_tail == next_buffer_head;
}


// Computes and returns block nominal speed based on running condition and override values.
// NOTE: All system motion commands, such as homing/parking, are not subject to overrides.
float plan_compute_profile_nominal_speed (plan_block_t *block)
{
    float nominal_speed = block->condition.spindle.synchronized ? block->programmed_rate * hal.spindle_get_data(SpindleData_RPM).rpm : block->programmed_rate;

    if (block->condition.rapid_motion)
        nominal_speed *= (0.01f * sys.r_override);
    else {
        if (!block->condition.no_feed_override)
            nominal_speed *= (0.01f * sys.f_override);
        if (nominal_speed > block->rapid_rate)
            nominal_speed = block->rapid_rate;
    }

// TODO: if nominal speed is outside bounds when synchronized motion is on then (?? retract and) abort, ignore overrides?
    return nominal_speed > MINIMUM_FEED_RATE ? nominal_speed : MINIMUM_FEED_RATE;
}


// Computes and updates the max entry speed (sqr) of the block, based on the minimum of the junction's
// previous and current nominal speeds and max junction speed.
inline static float plan_compute_profile_parameters (plan_block_t *block, float nominal_speed, float prev_nominal_speed)
{
  // Compute the junction maximum entry based on the minimum of the junction speed and neighboring nominal speeds.
    block->max_entry_speed_sqr = nominal_speed > prev_nominal_speed ? (prev_nominal_speed * prev_nominal_speed) : (nominal_speed * nominal_speed);
    if (block->max_entry_speed_sqr > block->max_junction_speed_sqr)
        block->max_entry_speed_sqr = block->max_junction_speed_sqr;
    return nominal_speed;
}

// Re-calculates buffered motions profile parameters upon a motion-based override change.
void plan_update_velocity_profile_parameters ()
{
    uint_fast8_t block_index = block_buffer_tail;
    plan_block_t *block;
    float prev_nominal_speed = SOME_LARGE_VALUE; // Set high for first block nominal speed calculation.

    while (block_index != block_buffer_head) {
        block = &block_buffer[block_index];
        prev_nominal_speed = plan_compute_profile_parameters(block, plan_compute_profile_nominal_speed(block), prev_nominal_speed);
        block_index = plan_next_block_index(block_index);
    }
    pl.previous_nominal_speed = prev_nominal_speed; // Update prev nominal speed for next incoming block.
}


/* Add a new linear movement to the buffer. target[N_AXIS] is the signed, absolute target position
   in millimeters. Feed rate specifies the speed of the motion. If feed rate is inverted, the feed
   rate is taken to mean "frequency" and would complete the operation in 1/feed_rate minutes.
   All position data passed to the planner must be in terms of machine position to keep the planner
   independent of any coordinate system changes and offsets, which are handled by the g-code parser.
   NOTE: Assumes buffer is available. Buffer checks are handled at a higher level by motion_control.
   In other words, the buffer head is never equal to the buffer tail.  Also the feed rate input value
   is used in three ways: as a normal feed rate if invert_feed_rate is false, as inverse time if
   invert_feed_rate is true, or as seek/rapids rate if the feed_rate value is negative (and
   invert_feed_rate always false).
   The system motion condition tells the planner to plan a motion in the always unused block buffer
   head. It avoids changing the planner state and preserves the buffer to ensure subsequent gcode
   motions are still planned correctly, while the stepper module only points to the block buffer head
   to execute the special system motion. */
bool plan_buffer_line (float *target, plan_line_data_t *pl_data)
{
    // Prepare and initialize new block. Copy relevant pl_data for block execution.
    plan_block_t *block = &block_buffer[block_buffer_head];
    int32_t target_steps[N_AXIS], position_steps[N_AXIS], delta_steps;
    uint_fast8_t idx;
    float unit_vec[N_AXIS];

    memset(block, 0, sizeof(plan_block_t));                         // Zero all block values.
    memcpy(&block->spindle, &pl_data->spindle, sizeof(spindle_t));  // Copy spindle data (RPM etc)
    block->condition.value = pl_data->condition.value;
    block->line_number = pl_data->line_number;
    block->message = pl_data->message;

    // Compute and store initial move distance data.

    // Copy position data based on type of motion being planned.
    if (block->condition.system_motion) {
   #ifdef COREXY
        position_steps[X_AXIS] = system_convert_corexy_to_x_axis_steps(sys_position);
        position_steps[Y_AXIS] = system_convert_corexy_to_y_axis_steps(sys_position);
        position_steps[Z_AXIS] = sys_position[Z_AXIS];
   #else
        memcpy(position_steps, sys_position, sizeof(sys_position));
   #endif
    } else
        memcpy(position_steps, pl.position, sizeof(pl.position));

  #ifdef COREXY
    target_steps[A_MOTOR] = lroundf(target[A_MOTOR] * settings.steps_per_mm[A_MOTOR]);
    target_steps[B_MOTOR] = lroundf(target[B_MOTOR] * settings.steps_per_mm[B_MOTOR]);
    int32_t a_steps = (target_steps[X_AXIS] - position_steps[X_AXIS]) + (target_steps[Y_AXIS] - position_steps[Y_AXIS]);
    int32_t b_steps = (target_steps[X_AXIS] - position_steps[X_AXIS]) - (target_steps[Y_AXIS] - position_steps[Y_AXIS]);
  #endif

    idx = N_AXIS;
    do {
        idx--;
        // Calculate target position in absolute steps, number of steps for each axis, and determine max step events.
        // Also, compute individual axes distance for move and prep unit vector calculations.
        // NOTE: Computes true distance from converted step values.
      #ifdef COREXY
        if(idx == A_MOTOR)
            delta_steps = a_steps;
        else if(idx == B_MOTOR)
            delta_steps = b_steps;
        else {
            target_steps[idx] = lround(target[idx] * settings.steps_per_mm[idx]);
            delta_steps = target_steps[idx] - position_steps[idx];
            break;
        }
      #else
        target_steps[idx] = lroundf(target[idx] * settings.steps_per_mm[idx]);
        delta_steps = target_steps[idx] - position_steps[idx];
      #endif

        block->steps[idx] = labs(delta_steps);
        block->step_event_count = max(block->step_event_count, block->steps[idx]);
        unit_vec[idx] = (float)delta_steps / settings.steps_per_mm[idx]; // Store unit vector numerator

        // Set direction bits. Bit enabled always means direction is negative.
        if (delta_steps < 0)
            block->direction_bits.mask |= bit(idx);

    } while(idx);

#ifdef CONSTANT_SURFACE_SPEED_OPTION
    // Calculate RPM to be used for Constant Surface Speed calculations
    if(block->condition.is_rpm_pos_adjusted) {
        block->spindle.rpm = block->spindle.surface_speed / (float)(position_steps[block->spindle.axis] / settings.steps_per_mm[block->spindle.axis] * 2.0f * M_PI);
        if(block->spindle.rpm > block->spindle.max_rpm)
            block->spindle.rpm = block->spindle.max_rpm;
        block->spindle.target_rpm = block->spindle.surface_speed / (target[block->spindle.axis] * 2.0f * M_PI);
        if(block->spindle.target_rpm > block->spindle.max_rpm)
            block->spindle.target_rpm = block->spindle.max_rpm;
    }
#endif

    // Bail if this is a zero-length block. Highly unlikely to occur.
    if (block->step_event_count == 0)
        return false;

    pl_data->message = NULL; // Indicate message is queued for display on execution

    // Calculate the unit vector of the line move and the block maximum feed rate and acceleration scaled
    // down such that no individual axes maximum values are exceeded with respect to the line direction.
    // NOTE: This calculation assumes all axes are orthogonal (Cartesian) and works with ABC-axes,
    // if they are also orthogonal/independent. Operates on the absolute value of the unit vector.
    block->millimeters = convert_delta_vector_to_unit_vector(unit_vec);
    block->acceleration = limit_value_by_axis_maximum(settings.acceleration, unit_vec);
    block->rapid_rate = limit_value_by_axis_maximum(settings.max_rate, unit_vec);

    // Store programmed rate.
    if (block->condition.rapid_motion)
        block->programmed_rate = block->rapid_rate;
    else {
        block->programmed_rate = pl_data->feed_rate;
        if (block->condition.inverse_time)
            block->programmed_rate *= block->millimeters;
    }

    // TODO: Need to check this method handling zero junction speeds when starting from rest.
    if ((block_buffer_head == block_buffer_tail) || (block->condition.system_motion)) {

        // Initialize block entry speed as zero. Assume it will be starting from rest. Planner will correct this later.
        // If system motion, the system motion block always is assumed to start from rest and end at a complete stop.
        block->entry_speed_sqr = 0.0f;
        block->max_junction_speed_sqr = 0.0f; // Starting from rest. Enforce start from zero velocity.

    } else {

        // Compute maximum allowable entry speed at junction by centripetal acceleration approximation.
        // Let a circle be tangent to both previous and current path line segments, where the junction
        // deviation is defined as the distance from the junction to the closest edge of the circle,
        // colinear with the circle center. The circular segment joining the two paths represents the
        // path of centripetal acceleration. Solve for max velocity based on max acceleration about the
        // radius of the circle, defined indirectly by junction deviation. This may be also viewed as
        // path width or max_jerk in the previous Grbl version. This approach does not actually deviate
        // from path, but used as a robust way to compute cornering speeds, as it takes into account the
        // nonlinearities of both the junction angle and junction velocity.
        //
        // NOTE: If the junction deviation value is finite, Grbl executes the motions in an exact path
        // mode (G61). If the junction deviation value is zero, Grbl will execute the motion in an exact
        // stop mode (G61.1) manner. In the future, if continuous mode (G64) is desired, the math here
        // is exactly the same. Instead of motioning all the way to junction point, the machine will
        // just follow the arc circle defined here. The Arduino doesn't have the CPU cycles to perform
        // a continuous mode path, but ARM-based microcontrollers most certainly do.
        //
        // NOTE: The max junction speed is a fixed value, since machine acceleration limits cannot be
        // changed dynamically during operation nor can the line move geometry. This must be kept in
        // memory in the event of a feedrate override changing the nominal speeds of blocks, which can
        // change the overall maximum entry speed conditions of all blocks.

        float junction_unit_vec[N_AXIS];
        float junction_cos_theta = 0.0f;

        idx = N_AXIS;
        do {
            idx--;
            junction_cos_theta -= pl.previous_unit_vec[idx] * unit_vec[idx];
            junction_unit_vec[idx] = unit_vec[idx] - pl.previous_unit_vec[idx];
        } while(idx);

        // NOTE: Computed without any expensive trig, sin() or acos(), by trig half angle identity of cos(theta).
        if (junction_cos_theta > 0.999999f)
            //  For a 0 degree acute junction, just set minimum junction speed.
            block->max_junction_speed_sqr = MINIMUM_JUNCTION_SPEED * MINIMUM_JUNCTION_SPEED;
        else if (junction_cos_theta < -0.999999f) {
            // Junction is a straight line or 180 degrees. Junction speed is infinite.
            block->max_junction_speed_sqr = SOME_LARGE_VALUE;
        } else {
            convert_delta_vector_to_unit_vector(junction_unit_vec);
            float junction_acceleration = limit_value_by_axis_maximum(settings.acceleration, junction_unit_vec);
            float sin_theta_d2 = sqrtf(0.5f * (1.0f - junction_cos_theta)); // Trig half angle identity. Always positive.
            block->max_junction_speed_sqr = max(MINIMUM_JUNCTION_SPEED * MINIMUM_JUNCTION_SPEED,
                                                  (junction_acceleration * settings.junction_deviation * sin_theta_d2) / (1.0f - sin_theta_d2));
        }
    }

    // Block system motion from updating this data to ensure next g-code motion is computed correctly.
    if (!block->condition.system_motion) {

        pl.previous_nominal_speed = plan_compute_profile_parameters(block, plan_compute_profile_nominal_speed(block), pl.previous_nominal_speed);

        // Update previous path unit_vector and planner position.
        memcpy(pl.previous_unit_vec, unit_vec, sizeof(unit_vec)); // pl.previous_unit_vec[] = unit_vec[]
        memcpy(pl.position, target_steps, sizeof(target_steps)); // pl.position[] = target_steps[]

        // New block is all set. Update buffer head and next buffer head indices.
        block_buffer_head = next_buffer_head;
        next_buffer_head = plan_next_block_index(block_buffer_head);

        // Finish up by recalculating the plan with the new block.
        planner_recalculate();
    }

    return true;
}


// Reset the planner position vectors. Called by the system abort/initialization routine.
void plan_sync_position ()
{
  // TODO: For motor configurations not in the same coordinate frame as the machine position,
  // this function needs to be updated to accomodate the difference.
   #ifdef COREXY
    uint_fast8_t idx = N_AXIS;
    do {
        switch(--idx) {
            case X_AXIS:
                pl.position[X_AXIS] = system_convert_corexy_to_x_axis_steps(sys_position);
                break;
            case Y_AXIS:
                pl.position[Y_AXIS] = system_convert_corexy_to_y_axis_steps(sys_position);
                break;
            default:
                pl.position[idx] = sys_position[idx];
                break;
        }
    } while (idx);
  #else
    memcpy(pl.position, sys_position, sizeof(pl.position));
  #endif
}


// Returns the number of available blocks are in the planner buffer.
uint8_t plan_get_block_buffer_available ()
{
    return (uint8_t)(block_buffer_head >= block_buffer_tail ? ((BLOCK_BUFFER_SIZE - 1) - (block_buffer_head - block_buffer_tail)) : (block_buffer_tail - block_buffer_head - 1));
}


// Re-initialize buffer plan with a partially completed block, assumed to exist at the buffer tail.
// Called after a steppers have come to a complete stop for a feed hold and the cycle is stopped.
void plan_cycle_reinitialize ()
{
    // Re-plan from a complete stop. Reset planner entry speeds and buffer planned pointer.
    st_update_plan_block_parameters();
    block_buffer_planned = block_buffer_tail;
    planner_recalculate();
}

// Set feed overrides
void plan_feed_override (uint_fast8_t feed_override, uint_fast8_t rapid_override)
{
    if(sys.override_ctrl.feed_rate_disable)
        return;

    feed_override = max(min(feed_override, MAX_FEED_RATE_OVERRIDE), MIN_FEED_RATE_OVERRIDE);

	if ((feed_override != sys.f_override) || (rapid_override != sys.r_override)) {
	  sys.f_override = (uint8_t)feed_override;
	  sys.r_override = (uint8_t)rapid_override;
	  sys.report.ovr_counter = 0; // Set to report change immediately
	  plan_update_velocity_profile_parameters();
	  plan_cycle_reinitialize();
	}
}