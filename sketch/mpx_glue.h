/* mpx_glue.h -- what sketch.ino needs from robot.cpp, without including robot.h
 * next to Arduino.h (Arduino's PI / HIGH / min ... macros break robot.h). */
#ifndef MPX_GLUE_H
#define MPX_GLUE_H
/* everything app_main() started on the ESP that this robot has: the robot
 * (servos, gait task, IMU), the battery reading, the face and its helpers */
void        mpx_glue_start();
const char *mpx_robot_mode();     /* the current gait's name */
bool        mpx_imu_ok();
#endif
