/** \file
	\brief Work out what to do with received G-Code commands
*/

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>

#include "bebopr.h"
#include "gcode_process.h"
#include "gcode_parse.h"
#include "debug.h"
#include "temp.h"
#include "heater.h"
#include "home.h"
#include "traject.h"
#include "pruss_stepper.h"
#include "heater.h"
#include "mendel.h"
#include "limit_switches.h"

/// the current tool
static uint8_t tool;
/// the tool to be changed when we get an M6
static uint8_t next_tool;

// 20111017 modmaker - use nanometers instead of steps for position
/// variable that holds the idea of 'current position' for the gcode interpreter.
/// the actual machine position will probably lag!
static TARGET gcode_current_pos;
//  Home Position holds the offset set by G92, it is used to convert the
//  gcode coordinates to machine / PRUSS coordinates.
static TARGET gcode_home_pos;
static double gcode_initial_feed;
/*
 * Local copy of channel tags to prevent a lookup with each access.
 */
static channel_tag heater_extruder = NULL;
static channel_tag heater_bed = NULL;
static channel_tag temp_extruder = NULL;
static channel_tag temp_bed = NULL;
static channel_tag pwm_extruder = NULL;

static int extruder_temp_wait = 0;
static int bed_temp_wait = 0;

/*
	private functions
*/

static void wait_for_slow_signals( void)
{
  if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
    printf( "defer move until temperature is stable!\n");
  }
  while ( (extruder_temp_wait && !heater_temp_reached( heater_extruder)) ||
	  (bed_temp_wait && !heater_temp_reached( heater_bed)) )
  {
    usleep( 100000);
  }
  extruder_temp_wait = 0;
  bed_temp_wait = 0;
  if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
    printf( "resume with move because temperature is stable!\n");
  }
}

/*
 *  make a move to new 'target' position, at the end of this move 'target'
 *  should reflect the actual position.
 */
static void enqueue_pos( TARGET* target)
{
  if (target != NULL) {
    if (extruder_temp_wait || bed_temp_wait) {
      wait_for_slow_signals();
    }
    if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
      printf( "enqueue_pos( TARGET={%d, %d, %d, %d, %u})\n",
	       target->X, target->Y, target->Z, target->E, target->F);
    }
#ifdef PRU_ABS_COORDS
    /* integer positions are in nm ! */ 
    traject5D traj = {
      .x0 = (double)1.0E-9 * (gcode_home_pos.X + gcode_current_pos.X),
      .y0 = (double)1.0E-9 * (gcode_home_pos.Y + gcode_current_pos.Y),
      .z0 = (double)1.0E-9 * (gcode_home_pos.Z + gcode_current_pos.Z),
      .e0 = (double)1.0E-9 * (gcode_home_pos.E + gcode_current_pos.E),
      .x1 = (double)1.0E-9 * (gcode_home_pos.X + target->X),
      .y1 = (double)1.0E-9 * (gcode_home_pos.Y + target->Y),
      .z1 = (double)1.0E-9 * (gcode_home_pos.Z + target->Z),
      .e1 = (double)1.0E-9 * (gcode_home_pos.E + target->E),
      .feed = target->F,
    };
#else
    /* integer positions are in nm ! */ 
    traject5D traj = {
      .dx = (double)1.0E-9 * (target->X - gcode_current_pos.X),
      .dy = (double)1.0E-9 * (target->Y - gcode_current_pos.Y),
      .dz = (double)1.0E-9 * (target->Z - gcode_current_pos.Z),
      .de = (double)1.0E-9 * (target->E - gcode_current_pos.E),
      .feed = target->F,
    };
#endif
    /* make the move */
    traject_delta_on_all_axes( &traj);
    /*
     * For a 3D printer, the E-axis controls the extruder and for that axis
     * the +/- 2000 mm operating range is not sufficient as this axis moves
     * mostly into one direction.
     * If this axis is configured to use relative coordinates only, after
     * each move the origin is shifted to the current position restoring the
     * full +/- 2000 mm operating range.
     */
    if (config_e_axis_is_always_relative()) {
      pruss_queue_adjust_origin( 4, gcode_home_pos.E + target->E);
      target->E = 0;	// target->E -= target->E;
    }
  }
}


/************************************************************************//**

  \brief Processes command stored in global \ref next_target.
  This is where we work out what to actually do with each command we
    receive. All data has already been scaled to integers in gcode_parse.
    If you want to add support for a new G or M code, this is the place.


*//*************************************************************************/


void clip_move( axis_e axis, int32_t* pnext_target, int32_t current_pos, int32_t home_pos)
{
	static const char axisNames[] = { 'X', 'Y', 'Z', 'E' };	// FIXME: this uses knowledge of axis_e
	double limit;
	if (*pnext_target >= current_pos) {
		// forward move or no move
		if (config_max_soft_limit( axis, &limit)) {
			int32_t pos_limit = MM2POS( limit);
			if (home_pos + current_pos > pos_limit) {
				pos_limit = home_pos + current_pos;
			}
			if (home_pos + *pnext_target > pos_limit) {
				printf( "WARNING: Clipping target.%c (%d) to %d due to upper soft limit= %d (home= %d)\n",
					axisNames[ axis], *pnext_target, pos_limit, MM2POS( limit), home_pos);
				*pnext_target = pos_limit - home_pos;
			}
		}	
	} else {
		// backward move
		if (config_min_soft_limit( axis, &limit)) {
			int32_t pos_limit = MM2POS( limit);
			if (home_pos + current_pos < pos_limit) {
				pos_limit = home_pos + current_pos;
			}
			if (home_pos + *pnext_target < pos_limit) {
				printf( "WARNING: Clipping target.%c (%d) to %d due to lower soft limit= %d (home= %d)\n",
					axisNames[ axis], *pnext_target, pos_limit, MM2POS( limit), home_pos);
				*pnext_target = pos_limit - home_pos;
			}
		}	
	}
}

void process_gcode_command() {
	uint32_t	backup_f;

	if (next_target.seen_F) {
		gcode_initial_feed = next_target.target.F;
	} else {
		next_target.target.F = gcode_initial_feed;
	}
	// convert relative to absolute
	if (next_target.option_relative) {
		next_target.target.X += gcode_current_pos.X;
		next_target.target.Y += gcode_current_pos.Y;
		next_target.target.Z += gcode_current_pos.Z;
		next_target.target.E += gcode_current_pos.E;
	}
	// The GCode documentation was taken from http://reprap.org/wiki/Gcode .

	if (next_target.seen_T) {
	    //? ==== T: Select Tool ====
	    //?
	    //? Example: T1
	    //?
	    //? Select extruder number 1 to build with.  Extruder numbering starts at 0.

	    next_tool = next_target.T;
	}

	// if we didn't see an axis word, set it to gcode_current_pos. this fixes incorrect moves after homing TODO: fix homing ???
//TODO: fix this ???
	if (next_target.seen_X == 0)
		next_target.target.X = gcode_current_pos.X;
	if (next_target.seen_Y == 0)
		next_target.target.Y = gcode_current_pos.Y;
	if (next_target.seen_Z == 0)
		next_target.target.Z = gcode_current_pos.Z;
	if (next_target.seen_E == 0)
		next_target.target.E = gcode_current_pos.E;

	if (next_target.seen_G) {
		uint8_t axisSelected = 0;
		switch (next_target.G) {
			// 	G0 - rapid, unsynchronised motion
			// since it would be a major hassle to force the dda to not synchronise, just provide a fast feedrate and hope it's close enough to what host expects
			case 0:
				//? ==== G0: Rapid move ====
				//?
				//? Example: G0 X12
				//?
				//? In this case move rapidly to X = 12 mm.  In fact, the RepRap firmware uses exactly the same code for rapid as it uses for controlled moves (see G1 below), as - for the RepRap machine - this is just as efficient as not doing so.  (The distinction comes from some old machine tools that used to move faster if the axes were not driven in a straight line.  For them G0 allowed any movement in space to get to the destination as fast as possible.)
			case 1:
			{
				//? ==== G1: Controlled move ====
				//?
				//? Example: G1 X90.6 Y13.8 E22.4
				//?
				//? Go in a straight line from the current (X, Y) point to the point (90.6, 13.8), extruding material as the move happens from the current extruded length to a length of 22.4 mm.
				/*
				 *  Implement soft axis limits:
				 *
				 *  The soft axis limits define a safe operating zone.
				 *  Coordinates are clipped in such a way that no moves are generate that would move
				 *  from the inside to the outside of the safe operating zone. All moves from outside
				 *  the safe operating zone directed towards the inside of the zone are allowed!
				 */
				if (next_target.seen_X) {
					clip_move( x_axis, &next_target.target.X, gcode_current_pos.X, gcode_home_pos.X);
				}
				if (next_target.seen_Y) {
					clip_move( y_axis, &next_target.target.Y, gcode_current_pos.Y, gcode_home_pos.Y);
				}
				if (next_target.seen_Z) {
					clip_move( z_axis, &next_target.target.Z, gcode_current_pos.Z, gcode_home_pos.Z);
				}

				if (next_target.G == 0) {
					backup_f = next_target.target.F;
					next_target.target.F = 100000;	// will be limited by the limitations of the individual axes
					enqueue_pos( &next_target.target);
					next_target.target.F = backup_f;
				} else {
					// synchronised motion
					enqueue_pos( &next_target.target);
				}
				/* update our sense of position */
				gcode_current_pos.X = next_target.target.X;
				gcode_current_pos.Y = next_target.target.Y;
				gcode_current_pos.Z = next_target.target.Z;
				gcode_current_pos.E = next_target.target.E;
				gcode_current_pos.F = next_target.target.F;
				break;
			}
				//	G2 - Arc Clockwise
				// unimplemented

				//	G3 - Arc Counter-clockwise
				// unimplemented

				//	G4 - Dwell
			case 4:
				//? ==== G4: Dwell ====
				//?
				//? Example: G4 P200
				//?
				//? In this case sit still doing nothing for 200 milliseconds.  During delays the state of the machine (for example the temperatures of its extruders) will still be preserved and controlled.
				//?

				traject_wait_for_completion();
				usleep( 1000* next_target.P);
				break;

				//	G20 - inches as units
			case 20:
				//? ==== G20: Set Units to Inches ====
				//?
				//? Example: G20
				//?
				//? Units from now on are in inches.
				//?
				next_target.option_inches = 1;
				break;

				//	G21 - mm as units
			case 21:
				//? ==== G21: Set Units to Millimeters ====
				//?
				//? Example: G21
				//?
				//? Units from now on are in millimeters.  (This is the RepRap default.)
				//?
				next_target.option_inches = 0;
				break;

				//	G30 - go home via point
			case 30:
				//? ==== G30: Go home via point ====
				//?
				//? Undocumented.
				enqueue_pos( &next_target.target);
				// no break here, G30 is move and then go home

				//	G28 - Move to Origin
			case 28:
				//? ==== G28: Move to Origin ====
				//?
				// 20110817 modmaker - Changed implementation according to info found in
				//                     these docs: http://linuxcnc.org/docs/html/gcode.html,
				// http://reprap.org/wiki/MCodeReference and http://reprap.org/wiki/GCodes .
				// G28 generates a rapid traversal to the origin (or a preset position).
				// Implementation: G0 like move with as destination the origin (x,y,z=0,0,0).
				// The (absolute) origin is set at startup (current position) or by executing
				// a calibration command (G161/G162) for one or more axes.
				if (next_target.seen_X) {
					next_target.target.X = 0;
					axisSelected = 1;
				}
				if (next_target.seen_Y) {
					next_target.target.Y = 0;
					axisSelected = 1;
				}
				if (next_target.seen_Z) {
					next_target.target.Z = 0;
					axisSelected = 1;
				}
				if (axisSelected != 1) {
					next_target.target.X = 0;
					next_target.target.Y = 0;
					next_target.target.Z = 0;
				}
				backup_f = next_target.target.F;
				next_target.target.F = 99999;		// let the software clip this to the maximum allowed rate
				enqueue_pos( &next_target.target);
				next_target.target.F = backup_f;
				break;

			//	G90 - absolute positioning
			case 90:
				//? ==== G90: Set to Absolute Positioning ====
				//?
				//? Example: G90
				//?
				//? All coordinates from now on are absolute relative to the origin of the machine.  (This is the RepRap default.)
				next_target.option_relative = 0;
				break;

				//	G91 - relative positioning
			case 91:
				//? ==== G91: Set to Relative Positioning ====
				//?
				//? Example: G91
				//?
				//? All coordinates from now on are relative to the last position.
				next_target.option_relative = 1;
				break;

				//	G92 - set home
			case 92:
				//? ==== G92: Set Position ====
				//?
				//? Example: G92 X10 E90
				//?
				//? Allows programming of absolute zero point, by reseting the current position to the values specified.  This would set the machine's X coordinate to 10, and the extrude coordinate to 90. No physical motion will occur.

				traject_wait_for_completion();

				if (next_target.seen_X) {
					gcode_home_pos.X += gcode_current_pos.X - next_target.target.X;
					gcode_current_pos.X = next_target.target.X;
					axisSelected = 1;
				}
				if (next_target.seen_Y) {
					gcode_home_pos.Y += gcode_current_pos.Y - next_target.target.Y;
					gcode_current_pos.Y = next_target.target.Y;
					axisSelected = 1;
				}
				if (next_target.seen_Z) {
					gcode_home_pos.Z += gcode_current_pos.Z - next_target.target.Z;
					gcode_current_pos.Z = next_target.target.Z;
					axisSelected = 1;
				}
				// TODO: this is exceptional, check wheter this doesn't clash 
				// with relative E axis operation !!!!
				if (next_target.seen_E) {
					if (!config_e_axis_is_always_relative() && next_target.target.E == 0) {
						// slicers use this te adjust the origin to prevent running
						// out of E range, adjust the PRUSS internal origin too.
						pruss_queue_adjust_origin( 4, gcode_home_pos.E + gcode_current_pos.E);
						// gcode_home_pos can overflow too, so clear it! NOTE: the E-axis
						// now doesn't behave like a normal (absolute) axis anymore!
						gcode_home_pos.E = 0;
					} else {
						gcode_home_pos.E += gcode_current_pos.E - next_target.target.E;
					}
					gcode_current_pos.E = next_target.target.E;
					axisSelected = 1;
				}
				if (axisSelected == 0) {
					gcode_home_pos.X += gcode_current_pos.X;
					gcode_current_pos.X = next_target.target.X = 0;
					gcode_home_pos.Y += gcode_current_pos.Y;
					gcode_current_pos.Y = next_target.target.Y = 0;
					gcode_home_pos.Z += gcode_current_pos.Z;
					gcode_current_pos.Z = next_target.target.Z = 0;
					gcode_home_pos.E += gcode_current_pos.E;
					gcode_current_pos.E = next_target.target.E = 0;
				}
				break;

#define FOR_ONE_AXIS( axis_lc, axis_uc, axis_i, code) \
	do {								\
		axis_xyz = axis_lc##_axis;				\
		pruss_axis_xyz = axis_i;				\
		next_target_seen_xyz = next_target.seen_##axis_uc;	\
		current_pos_xyz = gcode_current_pos.axis_uc;		\
		home_pos_xyz = gcode_home_pos.axis_uc;			\
		code							\
		gcode_current_pos.axis_uc = current_pos_xyz;		\
		gcode_home_pos.axis_uc = home_pos_xyz;			\
	} while (0)

#define FOR_EACH_AXIS_IN_XYZ( code) \
	do {								\
		uint32_t feed = next_target.target.F;			\
		axis_e axis_xyz;					\
		int pruss_axis_xyz;		   			\
		int next_target_seen_xyz;				\
		int32_t current_pos_xyz;				\
		int32_t home_pos_xyz;					\
		/* X */							\
		FOR_ONE_AXIS( x, X, 1, code);				\
		FOR_ONE_AXIS( y, Y, 2, code);				\
		FOR_ONE_AXIS( z, Z, 3, code);				\
	} while (0)

			// G161 - Home negative
			case 161:
			{
				//? ==== G161: Home negative ====
				//?
				//? Find the minimum limit of the specified axes by searching for the limit switch.
				// reference 'home' position to (then) current position

				// NOTE: G161/G162 clears any G92 offset !
				double pos;
				if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
					fprintf( stderr, "G161: X(%d)=%d, Y(%d)=%d, Z(%d)=%d, E(%d)=%d, F(%d)=%d\n",
						next_target.seen_X, next_target.target.X,
						next_target.seen_Y, next_target.target.Y,
						next_target.seen_Z, next_target.target.Z,
						next_target.seen_E, next_target.target.E,
						next_target.seen_F, next_target.target.F );
				}
				FOR_EACH_AXIS_IN_XYZ(
					if (next_target_seen_xyz) {
						// use machine coordinates during homing
						current_pos_xyz += home_pos_xyz;
						home_axis_to_min_limit_switch( axis_xyz, &current_pos_xyz, feed);
						// restore gcode coordinates
						current_pos_xyz -= home_pos_xyz;
						if (config_min_switch_pos( axis_xyz, &pos)) {
							home_pos_xyz = 0;
							current_pos_xyz = SI2POS( pos);
							pruss_queue_set_position( pruss_axis_xyz, home_pos_xyz + current_pos_xyz);
						}
					} );

				break;
			}
			// G162 - Home positive
			case 162:
			{
				//? ==== G162: Home positive ====
				//?
				//? Find the maximum limit of the specified axes by searching for the limit switch.
				// reference 'home' position to (then) current position
				double pos;
				if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
					fprintf( stderr, "G162: X(%d)=%d, Y(%d)=%d, Z(%d)=%d, E(%d)=%d, F(%d)=%d\n",
						next_target.seen_X, next_target.target.X,
						next_target.seen_Y, next_target.target.Y,
						next_target.seen_Z, next_target.target.Z,
						next_target.seen_E, next_target.target.E,
						next_target.seen_F, next_target.target.F );
				}
				FOR_EACH_AXIS_IN_XYZ(
					if (next_target_seen_xyz) {
						// use machine coordinates during homing
						current_pos_xyz += home_pos_xyz;
						home_axis_to_max_limit_switch( axis_xyz, &current_pos_xyz, feed);
						// restore gcode coordinates
						current_pos_xyz -= home_pos_xyz;
						if (config_max_switch_pos( axis_xyz, &pos)) {
							home_pos_xyz = 0;
							current_pos_xyz = SI2POS( pos);
							pruss_queue_set_position( pruss_axis_xyz, home_pos_xyz + current_pos_xyz);
						}
					} );
				break;
			}
			// G255 - Dump PRUSS state
			case 255:
				// === G255: Dump PRUSS state ====
				// The (optional) parameter S0, will disable waiting
				// for the current command to complete, before dumping.
				if (!next_target.seen_S || next_target.S != 0) {
				  traject_wait_for_completion();
				}
				pruss_stepper_dump_state();
				break;

				// unknown gcode: spit an error
			default:
				printf("E: Bad G-code %d", next_target.G);
				// newline is sent from gcode_parse after we return
				return;
		}
#ifdef	DEBUG
		if (DEBUG_POSITION && (debug_flags & DEBUG_POSITION)) {
			traject_status_print();
		}
#endif
	}
	else if (next_target.seen_M) {
		switch (next_target.M) {
			// M0- machine stop
			case 0:
			// M2- program end
			case 2:
				//? ==== M2: program end ====
				//?
				//? Undocumented.
				traject_wait_for_completion();
				// no break- we fall through to M112 below
			// M112- immediate stop
			case 112:
				//? ==== M112: Emergency Stop ====
				//?
				//? Example: M112
				//?
				//? Any moves in progress are immediately terminated, then RepRap shuts down.  All motors and heaters are turned off.
				//? It can be started again by pressing the reset button on the master microcontroller.  See also M0.

				traject_abort();

				x_disable();
				y_disable();
				z_disable();
				e_disable();
				power_off();
				for (;;) {
					sched_yield();
				}
				break;

			// M6- tool change
			case 6:
				//? ==== M6: tool change ====
				//?
				//? Undocumented.
				tool = next_tool;
				break;
			// M82- set extruder to absolute mode
			case 82: {
				int old_mode = config_set_e_axis_mode( 0);
				if (old_mode != 0 && DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
					fprintf( stderr, "G82: switching to absolute extruder coordinates\n");
				}
				break;
			}
			// M83- set extruder to relative mode
			case 83: {
				int old_mode = config_set_e_axis_mode( 1);
				if (old_mode == 0 && DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
					fprintf( stderr, "G83: switching to relative extruder coordinates\n");
				}
				break;
			}
			// M84- stop idle hold
			case 84:
				x_disable();
				y_disable();
				z_disable();
				e_disable();
				break;
			// M3/M101- extruder on
			case 3:
			case 101:
				//? ==== M101: extruder on ====
				//?
				//? Undocumented.
				#ifdef DC_EXTRUDER
					heater_set(DC_EXTRUDER, DC_EXTRUDER_PWM);
				#elif E_STARTSTOP_STEPS > 0
					do {
						// backup feedrate, move E very quickly then restore feedrate
						backup_f = gcode_current_pos.F;
						gcode_current_pos.F = MAXIMUM_FEEDRATE_E;
						SpecialMoveE( E_STARTSTOP_STEPS, MAXIMUM_FEEDRATE_E);
						gcode_current_pos.F = backup_f;
					} while (0);
				#endif
				break;

			// M102- extruder reverse

			// M5/M103- extruder off
			case 5:
			case 103:
				//? ==== M103: extruder off ====
				//?
				//? Undocumented.
				#ifdef DC_EXTRUDER
					heater_set(DC_EXTRUDER, 0);
				#elif E_STARTSTOP_STEPS > 0
					do {
						// backup feedrate, move E very quickly then restore feedrate
						backup_f = gcode_current_pos.F;
						gcode_current_pos.F = MAXIMUM_FEEDRATE_E;
						SpecialMoveE( E_STARTSTOP_STEPS, MAXIMUM_FEEDRATE_E);
						gcode_current_pos.F = backup_f;
					} while (0);
				#endif
				break;

			// M104- set temperature
			case 104:
				//? ==== M104: Set Extruder Temperature (Fast) ====
			case 140:
				//? ==== M140: Set heated bed temperature (Fast) ====
			case 109:
				//? ==== M109: Set Extruder Temperature (Wait) ====
			case 190:
				//? ==== M190: Set Bed Temperature (Wait)  ====
			{
				channel_tag heater;
				if (next_target.M == 140 || next_target.M == 190) {
					heater = heater_bed;
				} else {
					if (next_target.seen_P && next_target.P == 1) {
						heater = heater_bed;
					} else {
						heater = heater_extruder;
					}
				}
				if (next_target.seen_S) {
					heater_set_setpoint( heater, next_target.S);
					// if setpoint is not null, turn power on
					if (next_target.S > 0) {
						power_on();
						heater_enable( heater_extruder, 1);
					} else {
						heater_enable( heater_extruder, 0);
					}
				}
				if (next_target.M == 109 || next_target.M == 190) {
					if (heater == heater_bed) {
						bed_temp_wait = 1;
					} else {
						extruder_temp_wait = 1;
					}
				}
				break;
			}
			// M105- get temperature
			case 105: {
				//? ==== M105: Get Extruder Temperature ====
				//?
				//? Example: M105
				//?
				//? Request the temperature of the current extruder and the build base in degrees Celsius.  The temperatures are returned to the host computer.  For example, the line sent to the host in response to this command looks like
				//?
				//? <tt>ok T:201 B:117</tt>
				//?
				//? Teacup supports an optional P parameter as a sensor index to address.
				double celsius;
#				ifdef ENFORCE_ORDER
					// wait for all moves to complete
					traject_wait_for_completion();
#				endif
				if (next_target.seen_P) {
					channel_tag temp_source;
					switch (next_target.P) {
					case 0:  temp_source = heater_extruder; break;
					case 1:  temp_source = heater_bed; break;
					default: temp_source = NULL;
					}
					if (heater_get_celsius( temp_source, &celsius) == 0) {
						printf( "\nT:%1.1lf", celsius);
					}
				} else {
					heater_get_celsius( heater_extruder, &celsius);
					printf( "\nT:%1.1lf", celsius);
					if (heater_bed != NULL) {
						heater_get_celsius( heater_bed, &celsius);
						printf( " B:%1.1lf", celsius);
					}
				}
				break;
			}
			// M7/M106- fan on
			case 7:
			case 106:
				//? ==== M106: Fan On ====
				//?
				//? Example: M106
				//?
				//? Turn on the cooling fan (if any).

#				ifdef ENFORCE_ORDER
					// wait for all moves to complete
					traject_wait_for_completion();
#				endif
#				ifdef HEATER_FAN
					heater_set(HEATER_FAN, 255);
#				endif
				break;
			// M107- fan off
			case 9:
			case 107:
				//? ==== M107: Fan Off ====
				//?
				//? Example: M107
				//?
				//? Turn off the cooling fan (if any).

#				ifdef ENFORCE_ORDER
					// wait for all moves to complete
					traject_wait_for_completion();
#				endif
				#ifdef HEATER_FAN
					heater_set(HEATER_FAN, 0);
				#endif
				break;

			// M110- set line number
			case 110:
				//? ==== M110: Set Current Line Number ====
				//?
				//? Example: N123 M110
				//?
				//? Set the current line number to 123.  Thus the expected next line after this command will be 124.
				//? This is a no-op in Teacup.
				break;
			// M111- set debug level
			#ifdef	DEBUG
			case 111:
				//? ==== M111: Set Debug Level ====
				//?
				//? Example: M111 S6
				//?
				//? Set the level of debugging information transmitted back to the host to level 6.  The level is the OR of three bits:
				//?
				//? <Pre>
				//? #define         DEBUG_PID       1
				//? #define         DEBUG_DDA       2
				//? #define         DEBUG_POSITION  4
				//? </pre>
				//?
				//? This command is only available in DEBUG builds of Teacup.

				debug_flags = next_target.S;
				printf( "New debug_flags setting: 0x%04x\n", debug_flags);
				break;
			#endif
			// M113- extruder PWM
			case 113: {
				//? ==== M113: Set (extruder) PWM ====
				//?
				//? Example: M113 S0.125
				//?
				//? Set the (raw) extruder heater output to the specified value: 0.0-1.0 gives 0-100% duty cycle.
				//? Should only be used when there is no heater control loop configured for this output!!!
				if (next_target.seen_S) {
					pwm_set_output( pwm_extruder, next_target.S);
				}
				break;
			}
			// M114- report XYZEF to host
			case 114:
				//? ==== M114: Get Current Position ====
				//?
				//? Example: M114
				//?
				//? This causes the RepRap machine to report its current X, Y, Z and E coordinates to the host.
				//?
				//? For example, the machine returns a string such as:
				//?
				//? <tt>ok C: X:0.00 Y:0.00 Z:0.00 E:0.00</tt>
#				ifdef ENFORCE_ORDER
					// wait for all moves to complete
					traject_wait_for_completion();
#				endif
				printf(  "current: X=%1.6lf, Y=%1.6lf, Z=%1.6lf, E=%1.6lf, F=%d\n",
					POS2MM( gcode_current_pos.X), POS2MM( gcode_current_pos.Y),
					POS2MM( gcode_current_pos.Z), POS2MM( gcode_current_pos.E),
					gcode_current_pos.F);
				// newline is sent from gcode_parse after we return

				break;
			// M115- capabilities string
			case 115:
				//? ==== M115: Get Firmware Version and Capabilities ====
				//?
				//? Example: M115
				//?
				//? Request the Firmware Version and Capabilities of the current microcontroller
				//? The details are returned to the host computer as key:value pairs separated by spaces and terminated with a linefeed.
				//?
				//? sample data from firmware:
				//?  FIRMWARE_NAME:Teacup FIRMWARE_URL:http%%3A//github.com/triffid/Teacup_Firmware/ PROTOCOL_VERSION:1.0 MACHINE_TYPE:Mendel EXTRUDER_COUNT:1 TEMP_SENSOR_COUNT:1 HEATER_COUNT:1

				printf( "FIRMWARE_NAME: BeBoPr FIRMWARE_URL:https//github.com/modmaker/BeBoPr/ PROTOCOL_VERSION:1.0 MACHINE_TYPE:Mendel EXTRUDER_COUNT:%d TEMP_SENSOR_COUNT:%d HEATER_COUNT:%d", 1, 2, 2);
				// newline is sent from gcode_parse after we return
				break;
			// M116 - Wait for all temperatures and other slowly-changing variables to arrive at their set values.
			case 116: {
				//? ==== M116: Wait ====
				//?
				//? Example: M116
				//?
				//? Wait for ''all'' temperatures and other slowly-changing variables to arrive at their set values.  See also M109.

				double setpoint;
				// wait for all moves to complete
				traject_wait_for_completion();
				// wait for all (active) heaters to stabilize
				if (heater_get_setpoint( heater_extruder, &setpoint) == 0) {
					if (setpoint > 0.0) {
						extruder_temp_wait = 1;
					}
				}
				if (heater_get_setpoint( heater_bed, &setpoint) == 0) {
					if (setpoint > 0.0) {
						bed_temp_wait = 1;
					}
				}
				wait_for_slow_signals();
				break;
			}
			case 130:
				//? ==== M130: heater P factor ====
			case 131:
				//? ==== M131: heater I factor ====
			case 132:
				//? ==== M132: heater D factor ====
			case 133:
				//? ==== M133: heater I limit ====

				//? P0: set for extruder
				//? P1: set for bed
				//? Snnn.nn: factor to set
				if (next_target.seen_S) {
					pid_settings pid;
					channel_tag channel;
					if (next_target.seen_P) {
						switch (next_target.P) {
						case 0:  channel = heater_extruder; break;
						case 1:  channel = heater_bed; break;
						default: channel = NULL;
						}
					} else {
						channel = heater_extruder;
					}
					heater_get_pid_values( channel, &pid);
					switch (next_target.M) {
					case 130:	// M130- heater P factor
						pid.P = next_target.S;
						break;
					case 131:	// M131- heater I factor
						pid.I = next_target.S;
						break;
					case 132:	// M132- heater D factor
						pid.D = next_target.S;
						break;
					case 133:	// M133- heater I limit
						pid.I_limit = next_target.S;
						break;
					}
					heater_set_pid_values( channel, &pid);
				}
				break;
			// M134- save PID settings to eeprom
			case 134:
				//? ==== M134: save PID settings to eeprom ====
				//? Undocumented.
				heater_save_settings();
				break;
			// M135- set heater output
			case 135:
				//? ==== M135: set heater output ====
				//? Undocumented.
				if (next_target.seen_S) {
					channel_tag heater;
					switch (next_target.P) {
					case 0:  heater = heater_extruder; break;
					case 1:  heater = heater_bed; break;
					default: heater = NULL;
					}
					heater_set_raw_pwm( heater, next_target.S);
					power_on();
				}
				break;
			#ifdef	DEBUG
			// M136- PRINT PID settings to host
			case 136: {
				//? ==== M136: PRINT PID settings to host ====
				//? Undocumented.
				//? This comand is only available in DEBUG builds.
				pid_settings pid;
				channel_tag heater;
				if (next_target.seen_P) {
					switch (next_target.P) {
					case 0:  heater = heater_extruder; break;
					case 1:  heater = heater_bed; break;
					default: heater = NULL;
					}
				} else {
					heater = heater_extruder;
				}
				heater_get_pid_values( heater, &pid);
				printf( "P:%1.3f I:%1.3f D:%1.3f Ilim:%1.3f FF_factor:%1.3f FF_offset:%1.3f",
					pid.P, pid.I, pid.D, pid.I_limit, pid.FF_factor, pid.FF_offset);
				break;
			}
			#endif

			// M191- power off
			case 191:
				//? ==== M191: Power Off ====
				//? Undocumented.
#				ifdef ENFORCE_ORDER
					// wait for all moves to complete
					traject_wait_for_completion();
#				endif
				x_disable();
				y_disable();
				z_disable();
				e_disable();
				power_off();
				break;

			// M200 - report endstop status
			case 200:
			{
				//? ==== M200: report endstop status ====
				//? Report the current status of the endstops configured in the firmware to the host.
				int no_limit_switches = 1;
				if (config_axis_has_min_limit_switch( x_axis)) {
				    printf( "x_min:%d ", limsw_min( x_axis));
				    no_limit_switches = 0;
				}
				if (config_axis_has_max_limit_switch( x_axis)) {
				    printf( "x_max:%d ", limsw_max( x_axis));
				    no_limit_switches = 0;
				}
				if (config_axis_has_min_limit_switch( y_axis)) {
				    printf( "y_min:%d ", limsw_min( y_axis));
				    no_limit_switches = 0;
				}
				if (config_axis_has_max_limit_switch( y_axis)) {
				    printf( "y_max:%d ", limsw_max( y_axis));
				    no_limit_switches = 0;
				}
				if (config_axis_has_min_limit_switch( z_axis)) {
				    printf( "z_min:%d ", limsw_min( z_axis));
				    no_limit_switches = 0;
				}
				if (config_axis_has_max_limit_switch( z_axis)) {
				    printf( "z_max:%d ", limsw_max( z_axis));
				    no_limit_switches = 0;
				}
				if (no_limit_switches) {
				    printf("no endstops defined");
				}
				break;
			}
			// M207 - Calibrate reference switch position (Z-axis)
			case 207:
			{
				double pos;
				int min_max = 0;
				if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
					fprintf( stderr, "M207: Z axis known position <-> reference switch calibration\n");
				}
				// Clear home offset, specifief current_pos is in machine coordinates (???)
				// NOTE: the calculations that follow use home_pos (that is set to zero),
				//       do not optimize them as this shows the correct calculations!
				gcode_home_pos.Z = 0;
				if (next_target.seen_Z) {
					gcode_current_pos.Z = next_target.target.Z;
				} else {
					gcode_current_pos.Z = 0;
				}
				pruss_queue_set_position( 3, gcode_home_pos.Z + gcode_current_pos.Z);
				// use machine coordinates during homing
				gcode_current_pos.Z += gcode_home_pos.Z;
				if (config_max_switch_pos( z_axis, &pos)) {
					home_axis_to_max_limit_switch( z_axis, &gcode_current_pos.Z, next_target.target.F);
					min_max = 1;
				} else if (config_min_switch_pos( z_axis, &pos)) {
					home_axis_to_min_limit_switch( z_axis, &gcode_current_pos.Z, next_target.target.F);
					min_max = -1;
				}
				// restore gcode coordinates
				gcode_current_pos.Z -= gcode_home_pos.Z;
				if (min_max) {
					if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
						fprintf( stderr, "M207: update Z calibration switch position to: %lf [mm]\n",
							POS2MM( gcode_current_pos.Z));
					}
					// Clear home offset and set new calibration position
					config_set_cal_pos( z_axis, POS2SI( gcode_current_pos.Z));
					gcode_home_pos.Z = 0;
					pruss_queue_set_position( 3, gcode_home_pos.Z + gcode_current_pos.Z);
				}
				break;
			}
			case 220:
				//? ==== M220: speed override factor ====
			case 221:
				//? ==== M221: extruder override factor ====
				if (next_target.seen_S) {
					double old;
					double factor = 0.001 * next_target.S;
					if (factor < 0.001) {
						factor = 0.001;
					}
					if (next_target.M == 220) {
						old = traject_set_speed_override( factor);
					} else {
						old = traject_set_extruder_override( factor);
					}
					if (DEBUG_GCODE_PROCESS && (debug_flags & DEBUG_GCODE_PROCESS)) {
						fprintf( stderr, "M%d: set %s override factor to %1.3lf, old value was %1.3lf\n",
							next_target.M, (next_target.M == 221) ? "extruder" : "speed", factor, old);
					}
				}
				break;
			#ifdef	DEBUG
			// M240- echo off
			case 240:
				//? ==== M240: echo off ====
				//? Disable echo.
				//? This command is only available in DEBUG builds.
				debug_flags &= ~DEBUG_ECHO;
				printf( "Echo off");
				// newline is sent from gcode_parse after we return
				break;
				// M241- echo on
			case 241:
				//? ==== M241: echo on ====
				//? Enable echo.
				//? This command is only available in DEBUG builds.
				debug_flags |= DEBUG_ECHO;
				printf( "Echo on");
				// newline is sent from gcode_parse after we return
				break;

			// DEBUG: return current position, end position, queue
			case 250:
				//? ==== M250: return current position, end position, queue ====
				//? Undocumented
				//? This command is only available in DEBUG builds.
				printf(  "current: X=%1.6lf, Y=%1.6lf, Z=%1.6lf, E=%1.6lf, F=%d\n",
					POS2MM( gcode_current_pos.X), POS2MM( gcode_current_pos.Y),
					POS2MM( gcode_current_pos.Z), POS2MM( gcode_current_pos.E),
					gcode_current_pos.F);
				printf(  "origin: X=%1.6lf, Y=%1.6lf, Z=%1.6lf, E=%1.6lf\n",
					POS2MM( gcode_home_pos.X), POS2MM( gcode_home_pos.Y),
					POS2MM( gcode_home_pos.Z), POS2MM( gcode_home_pos.E));
				pruss_dump_position();
				break;

			// DEBUG: read arbitrary memory location
			case 253:
				//? ==== M253: read arbitrary memory location ====
				//? Undocumented
				//? This command is only available in DEBUG builds.

				// 2012-06-04 modmaker - not implemented, this is not an AVR!
				break;

			// DEBUG: write arbitrary memory location
			case 254:
				//? ==== M254: write arbitrary memory location ====
				//? Undocumented
				//? This command is only available in DEBUG builds.

				// 2012-06-04 modmaker - not implemented, this is not an AVR!
				break;
			#endif /* DEBUG */
				// unknown mcode: spit an error
			default:
				printf("E: Bad M-code %d", next_target.M);
				// newline is sent from gcode_parse after we return
		} // switch (next_target.M)
	} // else if (next_target.seen_M)
} // process_gcode_command()

int gcode_process_init( void)
{
  int result = mendel_sub_init( "traject", traject_init);
  if (result != 0) {
    return result;
  }
  heater_extruder = heater_lookup_by_name( "heater_extruder");
  heater_bed      = heater_lookup_by_name( "heater_bed");
  temp_extruder   = temp_lookup_by_name( "temp_extruder");
  temp_bed        = temp_lookup_by_name( "temp_bed");
  if (debug_flags & DEBUG_GCODE_PROCESS) {
    printf( "tag_name( heater_extruder) = '%s',  tag_name( heater_bed) = '%s',\n"
	    "tag_name( temp_extruder) = '%s',  tag_name( temp_bed) = '%s'\n",
	    tag_name( heater_extruder), tag_name( heater_bed),
	    tag_name( temp_extruder), tag_name( temp_bed));
  }
  pwm_extruder    = pwm_lookup_by_name( "pwm_laser_power");
  // If there's no extruder, or no laser power there's probably a configuration error!
  if ((heater_extruder == NULL || temp_extruder == NULL) && pwm_extruder == NULL) {
    return -1;
  }
  gcode_current_pos.X = gcode_home_pos.X = 0;
  gcode_current_pos.Y = gcode_home_pos.Y = 0;
  gcode_current_pos.Z = gcode_home_pos.Z = 0;
  gcode_current_pos.E = gcode_home_pos.E = 0;
  gcode_initial_feed  = 3000;
  return 0;
}
