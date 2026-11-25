/* mpx_glue.cpp -- see mpx_glue.h */
#include "robot.h"
#include "mpx_glue.h"
#include "battery.h"
#include "mpx_robot.h"
#include "imu.h"
#include "display_hw.h"
#include "display_glue.h"

void mpx_glue_start()
{
    /* the order of the ESP's app_main (main.cc) */
    display_hw_boot_status("Starting the servos");
    if (mpx_robot_init()) {            /* robot::init(): boards, range, soft start, gait task, IMU */
        mpx_battery_start();           /* the battery voltage from the servo boards, every 10 s */
    }
    display_glue::start_face();        /* the eyes replace the boot screen */
    display_hw_boot_done();
    display_glue::start_animations();  /* a looping GIF chosen before: back (the file comes from Linux) */
    display_glue::show_boot_check();   /* a servo / SPI problem found at boot: on the screen for 30 s */
    display_glue::start_temp_watch();  /* first minute > 60 C = faulty; later > 80 C = too high */
    display_glue::start_events();      /* the address line under the eyes */
}

bool mpx_imu_ok()
{
    const robot::ImuData d = robot::imu_read();
    return d.seq != 0;
}

const char *mpx_robot_mode()
{
    const char *n = robot::gait_to_name(robot::current_gait_cmd());
    return n ? n : "?";
}
