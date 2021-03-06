/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009, 2010 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

This file is part of the QGROUNDCONTROL project

    QGROUNDCONTROL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    QGROUNDCONTROL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with QGROUNDCONTROL. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

/**
 * @file
 *   @brief Implementation of simulated system link
 *
 *   @author Lorenz Meier <mavteam@student.ethz.ch>
 *
 */

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <cmath>
#include <QTime>
#include <QImage>
#include <QDebug>
#include <QFileInfo>
#include "LinkManager.h"
#include "MAVLinkProtocol.h"
#include "MAVLinkSimulationLink.h"
#include "QGCMAVLink.h"
#include "QGC.h"
#include "MAVLinkSimulationMAV.h"

/**
 * Create a simulated link. This link is connected to an input and output file.
 * The link sends one line at a time at the specified sendrate. The timing of
 * the sendrate is free of drift, which means it is stable on the long run.
 * However, small deviations are mixed in to vary the sendrate slightly
 * in order to simulate disturbances on a real communication link.
 *
 * @param readFile The file with the messages to read (must be in ASCII format, line breaks can be Unix or Windows style)
 * @param writeFile The received messages are written to that file
 * @param rate The rate at which the messages are sent (in intervals of milliseconds)
 **/
MAVLinkSimulationLink::MAVLinkSimulationLink(QString readFile, QString writeFile, int rate) :
    readyBytes(0),
    timeOffset(0)
{
    this->rate = rate;
    _isConnected = false;

    onboardParams = QMap<QString, float>();
    onboardParams.insert("PID_ROLL_K_P", 0.5f);
    onboardParams.insert("PID_PITCH_K_P", 0.5f);
    onboardParams.insert("PID_YAW_K_P", 0.5f);
    onboardParams.insert("PID_XY_K_P", 100.0f);
    onboardParams.insert("PID_ALT_K_P", 0.5f);
    onboardParams.insert("SYS_TYPE", 1);
    onboardParams.insert("SYS_ID", systemId);
    onboardParams.insert("RC4_REV", 0);
    onboardParams.insert("RC5_REV", 1);
    onboardParams.insert("HDNG2RLL_P", 0.7f);
    onboardParams.insert("HDNG2RLL_I", 0.01f);
    onboardParams.insert("HDNG2RLL_D", 0.02f);
    onboardParams.insert("HDNG2RLL_IMAX", 500.0f);
    onboardParams.insert("RLL2SRV_P", 0.4f);
    onboardParams.insert("RLL2SRV_I", 0.0f);
    onboardParams.insert("RLL2SRV_D", 0.0f);
    onboardParams.insert("RLL2SRV_IMAX", 500.0f);

    // Comments on the variables can be found in the header file

    simulationFile = new QFile(readFile, this);
    if (simulationFile->exists() && simulationFile->open(QIODevice::ReadOnly | QIODevice::Text))
    {
        simulationHeader = simulationFile->readLine();
    }
    receiveFile = new QFile(writeFile, this);
    lastSent = QGC::groundTimeMilliseconds();

    if (simulationFile->exists())
    {
        this->name = "Simulation: " + QFileInfo(simulationFile->fileName()).fileName();
    }
    else
    {
        this->name = "MAVLink simulation link";
    }



    // Initialize the pseudo-random number generator
    srand(QTime::currentTime().msec());
    maxTimeNoise = 0;
    this->id = getNextLinkId();
    LinkManager::instance()->_addLink(this);
}

MAVLinkSimulationLink::~MAVLinkSimulationLink()
{
    //TODO Check destructor
    //    fileStream->flush();
    //    outStream->flush();
    // Force termination, there is no
    // need for cleanup since
    // this thread is not manipulating
    // any relevant data
    terminate();
    delete simulationFile;
}

void MAVLinkSimulationLink::run()
{

    status.voltage_battery = 0;
    status.errors_comm = 0;

    system.base_mode = MAV_MODE_PREFLIGHT;
    system.custom_mode = MAV_MODE_FLAG_MANUAL_INPUT_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED;
    system.system_status = MAV_STATE_UNINIT;

    forever
    {
        static quint64 last = 0;

        if (QGC::groundTimeMilliseconds() - last >= rate)
        {
            if (_isConnected)
            {
                mainloop();
                readBytes();
            }
            else
            {
                // Sleep for substantially longer
                // if not connected
                QGC::SLEEP::msleep(500);
            }
            last = QGC::groundTimeMilliseconds();
        }
        QGC::SLEEP::msleep(3);
    }
}

void MAVLinkSimulationLink::sendMAVLinkMessage(const mavlink_message_t* msg)
{
    // Allocate buffer with packet data
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    unsigned int bufferlength = mavlink_msg_to_send_buffer(buf, msg);

    // Pack to link buffer
    readyBufferMutex.lock();
    for (unsigned int i = 0; i < bufferlength; i++)
    {
        readyBuffer.enqueue(*(buf + i));
    }
    readyBufferMutex.unlock();
}

void MAVLinkSimulationLink::enqueue(uint8_t* stream, uint8_t* index, mavlink_message_t* msg)
{
    // Allocate buffer with packet data
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    unsigned int bufferlength = mavlink_msg_to_send_buffer(buf, msg);
    //add data into datastream
    memcpy(stream+(*index),buf, bufferlength);
    (*index) += bufferlength;
}

void MAVLinkSimulationLink::mainloop()
{

    // Test for encoding / decoding packets

    // Test data stream
    streampointer = 0;

    // Fake system values

    static float fullVoltage = 4.2f * 3.0f;
    static float emptyVoltage = 3.35f * 3.0f;
    static float voltage = fullVoltage;
    static float drainRate = 0.025f; // x.xx% of the capacity is linearly drained per second

    mavlink_attitude_t attitude;
    memset(&attitude, 0, sizeof(mavlink_attitude_t));
    mavlink_raw_imu_t rawImuValues;
    memset(&rawImuValues, 0, sizeof(mavlink_raw_imu_t));

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    int bufferlength;
    mavlink_message_t msg;

    // Timers
    static unsigned int rate1hzCounter = 1;
    static unsigned int rate10hzCounter = 1;
    static unsigned int rate50hzCounter = 1;
    static unsigned int circleCounter = 0;

    // Vary values

    // VOLTAGE
    // The battery is drained constantly
    voltage = voltage - ((fullVoltage - emptyVoltage) * drainRate / rate);
    if (voltage < 3.550f * 3.0f) voltage = 3.550f * 3.0f;

    static int state = 0;

    if (state == 0)
    {
        state++;
    }


    // 50 HZ TASKS
    if (rate50hzCounter == 1000 / rate / 40)
    {
        if (simulationFile->isOpen())
        {
            if (simulationFile->atEnd()) {
                // We reached the end of the file, start from scratch
                simulationFile->reset();
                simulationHeader = simulationFile->readLine();
            }

            // Data was made available, read one line
            // first entry is the timestamp
            QString values = QString(simulationFile->readLine());
            QStringList parts = values.split("\t");
            QStringList keys = simulationHeader.split("\t");
            //qDebug() << simulationHeader;
            //qDebug() << values;
            bool ok;
            static quint64 lastTime = 0;
            static quint64 baseTime = 0;
            quint64 time = QString(parts.first()).toLongLong(&ok, 10);
            // FIXME Remove multiplicaton by 1000
            time *= 1000;

            if (ok) {
                if (timeOffset == 0) {
                    timeOffset = time;
                    baseTime = time;
                }

                if (lastTime > time) {
                    // We have wrapped around in the logfile
                    // Add the measurement time interval to the base time
                    baseTime += lastTime - timeOffset;
                }
                lastTime = time;

                time = time - timeOffset + baseTime;

                // Gather individual measurement values
                for (int i = 1; i < (parts.size() - 1); ++i) {
                    // Get one data field
                    bool res;
                    double d = QString(parts.at(i)).toDouble(&res);
                    if (!res) d = 0;

                    if (keys.value(i, "") == "Accel._X") {
                        rawImuValues.xacc = d;
                    }

                    if (keys.value(i, "") == "Accel._Y") {
                        rawImuValues.yacc = d;
                    }

                    if (keys.value(i, "") == "Accel._Z") {
                        rawImuValues.zacc = d;
                    }
                    if (keys.value(i, "") == "Gyro_Phi") {
                        rawImuValues.xgyro = d;
                        attitude.rollspeed = ((d-29.000)/15000.0)*2.7-2.7-2.65;
                    }

                    if (keys.value(i, "") == "Gyro_Theta") {
                        rawImuValues.ygyro = d;
                        attitude.pitchspeed = ((d-29.000)/15000.0)*2.7-2.7-2.65;
                    }

                    if (keys.value(i, "") == "Gyro_Psi") {
                        rawImuValues.zgyro = d;
                        attitude.yawspeed = ((d-29.000)/3000.0)*2.7-2.7-2.65;
                    }
                    if (keys.value(i, "") == "roll_IMU") {
                        attitude.roll = d;
                    }

                    if (keys.value(i, "") == "pitch_IMU") {
                        attitude.pitch = d;
                    }

                    if (keys.value(i, "") == "yaw_IMU") {
                        attitude.yaw = d;
                    }

                    //Accel._X	Accel._Y	Accel._Z	Battery	Bottom_Rotor	CPU_Load	Ground_Dist.	Gyro_Phi	Gyro_Psi	Gyro_Theta	Left_Servo	Mag._X	Mag._Y	Mag._Z	Pressure	Right_Servo	Temperature	Top_Rotor	pitch_IMU	roll_IMU	yaw_IMU

                }
                // Send out packets


                // ATTITUDE
                attitude.time_boot_ms = time/1000;
                // Pack message and get size of encoded byte string
                mavlink_msg_attitude_encode(systemId, componentId, &msg, &attitude);
                // Allocate buffer with packet data
                bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                //add data into datastream
                memcpy(stream+streampointer,buffer, bufferlength);
                streampointer += bufferlength;

                // IMU
                rawImuValues.time_usec = time;
                rawImuValues.xmag = 0;
                rawImuValues.ymag = 0;
                rawImuValues.zmag = 0;
                // Pack message and get size of encoded byte string
                mavlink_msg_raw_imu_encode(systemId, componentId, &msg, &rawImuValues);
                // Allocate buffer with packet data
                bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                //add data into datastream
                memcpy(stream+streampointer,buffer, bufferlength);
                streampointer += bufferlength;

                //qDebug() << "ATTITUDE" << "BUF LEN" << bufferlength << "POINTER" << streampointer;

                //qDebug() << "REALTIME" << QGC::groundTimeMilliseconds() << "ONBOARDTIME" << attitude.msec << "ROLL" << attitude.roll;

            }

        }

        rate50hzCounter = 1;
    }


    // 10 HZ TASKS
    if (rate10hzCounter == 1000 / rate / 9) {
        rate10hzCounter = 1;

        double lastX = x;
        double lastY = y;
        double lastZ = z;
        double hackDt = 0.1f; // 100 ms

        // Move X Position
        x = 12.0*sin(((double)circleCounter)/200.0);
        y = 5.0*cos(((double)circleCounter)/200.0);
        z = 1.8 + 1.2*sin(((double)circleCounter)/200.0);

        double xSpeed = (x - lastX)/hackDt;
        double ySpeed = (y - lastY)/hackDt;
        double zSpeed = (z - lastZ)/hackDt;



        circleCounter++;


//        x = (x > 5.0f) ? 5.0f : x;
//        y = (y > 5.0f) ? 5.0f : y;
//        z = (z > 3.0f) ? 3.0f : z;

//        x = (x < -5.0f) ? -5.0f : x;
//        y = (y < -5.0f) ? -5.0f : y;
//        z = (z < -3.0f) ? -3.0f : z;

        mavlink_message_t ret;

        // Send back new position
        mavlink_msg_local_position_ned_pack(systemId, componentId, &ret, 0, x, y, -fabs(z), xSpeed, ySpeed, zSpeed);
        bufferlength = mavlink_msg_to_send_buffer(buffer, &ret);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

//        // GPS RAW
//        mavlink_msg_gps_raw_pack(systemId, componentId, &ret, 0, 3, 47.376417+(x*0.00001), 8.548103+(y*0.00001), z, 0, 0, 2.5f, 0.1f);
//        bufferlength = mavlink_msg_to_send_buffer(buffer, &ret);
//        //add data into datastream
//        memcpy(stream+streampointer,buffer, bufferlength);
//        streampointer += bufferlength;

        // GLOBAL POSITION
        mavlink_msg_global_position_int_pack(systemId, componentId, &ret, 0, (473780.28137103+(x))*1E3, (85489.9892510421+(y))*1E3, (z+550.0)*1000.0, (z+550.0)*1000.0-1, xSpeed, ySpeed, zSpeed, yaw);
        bufferlength = mavlink_msg_to_send_buffer(buffer, &ret);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

        // GLOBAL POSITION VEHICLE 2
        mavlink_msg_global_position_int_pack(systemId+1, componentId+1, &ret, 0, (473780.28137103+(x+0.00001))*1E3, (85489.9892510421+((y/2)+0.00001))*1E3, (z+550.0)*1000.0, (z+550.0)*1000.0-1, xSpeed, ySpeed, zSpeed, yaw);
        bufferlength = mavlink_msg_to_send_buffer(buffer, &ret);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

//        // ATTITUDE VEHICLE 2
//        mavlink_msg_attitude_pack(54, MAV_COMP_ID_IMU, &ret, 0, 0, 0, atan2((y/2)+0.3, (x+0.002)), 0, 0, 0);
//        sendMAVLinkMessage(&ret);


//        // GLOBAL POSITION VEHICLE 3
//        mavlink_msg_global_position_int_pack(60, componentId, &ret, 0, (473780.28137103+(x/2+0.002))*1E3, (85489.9892510421+((y*2)+0.3))*1E3, (z+590.0)*1000.0, 0*100.0, 0*100.0, 0*100.0);
//        bufferlength = mavlink_msg_to_send_buffer(buffer, &ret);
//        //add data into datastream
//        memcpy(stream+streampointer,buffer, bufferlength);
//        streampointer += bufferlength;

        static int rcCounter = 0;
        if (rcCounter == 2) {
            mavlink_rc_channels_t chan;
            chan.time_boot_ms = 0;
            chan.chan1_raw = 1000 + ((int)(fabs(x) * 1000) % 2000);
            chan.chan2_raw = 1000 + ((int)(fabs(y) * 1000) % 2000);
            chan.chan3_raw = 1000 + ((int)(fabs(z) * 1000) % 2000);
            chan.chan4_raw = (chan.chan1_raw + chan.chan2_raw) / 2.0f;
            chan.chan5_raw = (chan.chan3_raw + chan.chan4_raw) / 2.0f;
            chan.chan6_raw = (chan.chan3_raw + chan.chan2_raw) / 2.0f;
            chan.chan7_raw = (chan.chan4_raw + chan.chan2_raw) / 2.0f;
            chan.chan8_raw = 0;
            chan.rssi = 100;
            mavlink_msg_rc_channels_encode(systemId, componentId, &msg, &chan);
            // Allocate buffer with packet data
            bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
            //add data into datastream
            memcpy(stream+streampointer,buffer, bufferlength);
            streampointer += bufferlength;
            rcCounter = 0;
        }
        rcCounter++;

    }

    // 1 HZ TASKS
    if (rate1hzCounter == 1000 / rate / 1) {
        // STATE
        static int statusCounter = 0;
        if (statusCounter == 100) {
            system.base_mode = (system.base_mode + 1) % MAV_MODE_ENUM_END;
            statusCounter = 0;
        }
        statusCounter++;

        static int detectionCounter = 6;
        if (detectionCounter % 10 == 0) {
        }
        detectionCounter++;

        status.voltage_battery = voltage * 1000; // millivolts
        status.load = 33 * detectionCounter % 1000;

        // Pack message and get size of encoded byte string
        mavlink_msg_sys_status_encode(systemId, componentId, &msg, &status);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

        // Pack debug text message
        mavlink_statustext_t text;
        text.severity = 0;
        strcpy((char*)(text.text), "Text message from system 32");
        mavlink_msg_statustext_encode(systemId, componentId, &msg, &text);
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        memcpy(stream+streampointer, buffer, bufferlength);
        streampointer += bufferlength;

        /*
        // Pack message and get size of encoded byte string
        mavlink_msg_boot_pack(systemId, componentId, &msg, version);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;*/

        // HEARTBEAT

        static int typeCounter = 0;
        uint8_t mavType;
        if (typeCounter < 10)
        {
            mavType = MAV_TYPE_QUADROTOR;
        }
        else
        {
            mavType = typeCounter % (MAV_TYPE_GCS);
        }
        typeCounter++;

        // Pack message and get size of encoded byte string
        mavlink_msg_heartbeat_pack(systemId, componentId, &msg, mavType, MAV_AUTOPILOT_PIXHAWK, system.base_mode, system.custom_mode, system.system_status);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //qDebug() << "CRC:" << msg.ck_a << msg.ck_b;
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

        // Pack message and get size of encoded byte string
        mavlink_msg_heartbeat_pack(systemId+1, componentId+1, &msg, mavType, MAV_AUTOPILOT_GENERIC, system.base_mode, system.custom_mode, system.system_status);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //qDebug() << "CRC:" << msg.ck_a << msg.ck_b;
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;


        // Send controller states

        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        memcpy(stream+streampointer, buffer, bufferlength);
        streampointer += bufferlength;

        // Pack message and get size of encoded byte string
        mavlink_msg_sys_status_encode(54, componentId, &msg, &status);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

        rate1hzCounter = 1;
    }

    // FULL RATE TASKS
    // Default is 50 Hz

    /*
    // 50 HZ TASKS
    if (rate50hzCounter == 1000 / rate / 50)
    {

        //streampointer = 0;

        // Attitude

        // Pack message and get size of encoded byte string
        mavlink_msg_attitude_pack(systemId, componentId, &msg, usec, roll, pitch, yaw, 0, 0, 0);
        // Allocate buffer with packet data
        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
        //add data into datastream
        memcpy(stream+streampointer,buffer, bufferlength);
        streampointer += bufferlength;

        rate50hzCounter = 1;
    }*/

    readyBufferMutex.lock();
    for (unsigned int i = 0; i < streampointer; i++) {
        readyBuffer.enqueue(*(stream + i));
    }
    readyBufferMutex.unlock();

    // Increment counters after full main loop
    rate1hzCounter++;
    rate10hzCounter++;
    rate50hzCounter++;
}


void MAVLinkSimulationLink::writeBytes(const char* data, qint64 size)
{
    // Parse bytes
    mavlink_message_t msg;
    mavlink_status_t comm;

    uint8_t stream[2048];
    int streampointer = 0;
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    int bufferlength = 0;
    
    // Initialize drop count to 0 so it isn't referenced uninitialized when returned at the bottom of this function
    comm.packet_rx_drop_count = 0;

    // Output all bytes as hex digits
    for (int i=0; i<size; i++)
    {
        if (mavlink_parse_char(this->id, data[i], &msg, &comm))
        {
            // MESSAGE RECEIVED!
//            qDebug() << "SIMULATION LINK RECEIVED MESSAGE!";
            emit messageReceived(msg);

            switch (msg.msgid)
            {
                // SET THE SYSTEM MODE
            case MAVLINK_MSG_ID_SET_MODE:
            {
                mavlink_set_mode_t mode;
                mavlink_msg_set_mode_decode(&msg, &mode);
                // Set mode indepent of mode.target
                system.base_mode = mode.base_mode;
            }
            break;
            // EXECUTE OPERATOR ACTIONS
            case MAVLINK_MSG_ID_COMMAND_LONG:
            {
                mavlink_command_long_t action;
                mavlink_msg_command_long_decode(&msg, &action);

//                qDebug() << "SIM" << "received action" << action.command << "for system" << action.target_system;

                // FIXME MAVLINKV10PORTINGNEEDED
//                switch (action.action) {
//                case MAV_ACTION_LAUNCH:
//                    status.status = MAV_STATE_ACTIVE;
//                    status.mode = MAV_MODE_AUTO;
//                    break;
//                case MAV_ACTION_RETURN:
//                    status.status = MAV_STATE_ACTIVE;
//                    break;
//                case MAV_ACTION_MOTORS_START:
//                    status.status = MAV_STATE_ACTIVE;
//                    status.mode = MAV_MODE_LOCKED;
//                    break;
//                case MAV_ACTION_MOTORS_STOP:
//                    status.status = MAV_STATE_STANDBY;
//                    status.mode = MAV_MODE_LOCKED;
//                    break;
//                case MAV_ACTION_EMCY_KILL:
//                    status.status = MAV_STATE_EMERGENCY;
//                    status.mode = MAV_MODE_MANUAL;
//                    break;
//                case MAV_ACTION_SHUTDOWN:
//                    status.status = MAV_STATE_POWEROFF;
//                    status.mode = MAV_MODE_LOCKED;
//                    break;
//                }
            }
            break;
            case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
            {
//                qDebug() << "GCS REQUESTED PARAM LIST FROM SIMULATION";
                mavlink_param_request_list_t read;
                mavlink_msg_param_request_list_decode(&msg, &read);
                if (read.target_system == systemId)
                {
                    // Output all params
                    // Iterate through all components, through all parameters and emit them
                    QMap<QString, float>::iterator i;
                    // Iterate through all components / subsystems
                    int j = 0;
                    for (i = onboardParams.begin(); i != onboardParams.end(); ++i) {
                        if (j != 5) {
                            // Pack message and get size of encoded byte string
                            mavlink_msg_param_value_pack(read.target_system, componentId, &msg, i.key().toStdString().c_str(), i.value(), MAV_PARAM_TYPE_REAL32, onboardParams.size(), j);
                            // Allocate buffer with packet data
                            bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                            //add data into datastream
                            memcpy(stream+streampointer,buffer, bufferlength);
                            streampointer+=bufferlength;
                        }
                        j++;
                    }

//                    qDebug() << "SIMULATION SENT PARAMETERS TO GCS";
                }
            }
                break;
            case MAVLINK_MSG_ID_PARAM_SET:
            {
                // Drop on even milliseconds
                if (QGC::groundTimeMilliseconds() % 2 == 0)
                {
//                    qDebug() << "SIMULATION RECEIVED COMMAND TO SET PARAMETER";
                    mavlink_param_set_t set;
                    mavlink_msg_param_set_decode(&msg, &set);
                    //                    if (set.target_system == systemId)
                    //                    {
                    QString key = QString((char*)set.param_id);
                    if (onboardParams.contains(key))
                    {
                        onboardParams.remove(key);
                        onboardParams.insert(key, set.param_value);

                        // Pack message and get size of encoded byte string
                        mavlink_msg_param_value_pack(set.target_system, componentId, &msg, key.toStdString().c_str(), set.param_value, MAV_PARAM_TYPE_REAL32, onboardParams.size(), onboardParams.keys().indexOf(key));
                        // Allocate buffer with packet data
                        bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                        //add data into datastream
                        memcpy(stream+streampointer,buffer, bufferlength);
                        streampointer+=bufferlength;
                    }
                    //                    }
                }
            }
            break;
            case MAVLINK_MSG_ID_PARAM_REQUEST_READ:
            {
//                qDebug() << "SIMULATION RECEIVED COMMAND TO SEND PARAMETER";
                mavlink_param_request_read_t read;
                mavlink_msg_param_request_read_decode(&msg, &read);
                QByteArray bytes((char*)read.param_id, MAVLINK_MSG_PARAM_REQUEST_READ_FIELD_PARAM_ID_LEN);
                QString key = QString(bytes);
                if (onboardParams.contains(key))
                {
                    float paramValue = onboardParams.value(key);

                    // Pack message and get size of encoded byte string
                    mavlink_msg_param_value_pack(read.target_system, componentId, &msg, key.toStdString().c_str(), paramValue, MAV_PARAM_TYPE_REAL32, onboardParams.size(), onboardParams.keys().indexOf(key));
                    // Allocate buffer with packet data
                    bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                    //add data into datastream
                    memcpy(stream+streampointer,buffer, bufferlength);
                    streampointer+=bufferlength;
                    //qDebug() << "Sending PARAM" << key;
                }
                else if (read.param_index >= 0 && read.param_index < onboardParams.keys().size())
                {
                    key = onboardParams.keys().at(read.param_index);
                    float paramValue = onboardParams.value(key);

                    // Pack message and get size of encoded byte string
                    mavlink_msg_param_value_pack(read.target_system, componentId, &msg, key.toStdString().c_str(), paramValue, MAV_PARAM_TYPE_REAL32, onboardParams.size(), onboardParams.keys().indexOf(key));
                    // Allocate buffer with packet data
                    bufferlength = mavlink_msg_to_send_buffer(buffer, &msg);
                    //add data into datastream
                    memcpy(stream+streampointer,buffer, bufferlength);
                    streampointer+=bufferlength;
                    //qDebug() << "Sending PARAM #ID" << (read.param_index) << "KEY:" << key;
                }
            }
            break;
            }
        }
    }

    // Log the amount and time written out for future data rate calculations.
    // While this interface doesn't actually write any data to external systems,
    // this data "transmit" here should still count towards the outgoing data rate.
    QMutexLocker dataRateLocker(&dataRateMutex);
    logDataRateToBuffer(outDataWriteAmounts, outDataWriteTimes, &outDataIndex, size, QDateTime::currentMSecsSinceEpoch());

    readyBufferMutex.lock();
    for (int i = 0; i < streampointer; i++)
    {
        readyBuffer.enqueue(*(stream + i));
    }
    readyBufferMutex.unlock();

    // Update comm status
    status.errors_comm = comm.packet_rx_drop_count;

}


void MAVLinkSimulationLink::readBytes()
{
    // Lock concurrent resource readyBuffer
    readyBufferMutex.lock();
    const qint64 maxLength = 2048;
    char data[maxLength];
    qint64 len = qMin((qint64)readyBuffer.size(), maxLength);

    for (unsigned int i = 0; i < len; i++) {
        *(data + i) = readyBuffer.takeFirst();
    }

    QByteArray b(data, len);
    emit bytesReceived(this, b);
    readyBufferMutex.unlock();

    // Log the amount and time received for future data rate calculations.
    QMutexLocker dataRateLocker(&dataRateMutex);
    logDataRateToBuffer(inDataWriteAmounts, inDataWriteTimes, &inDataIndex, len, QDateTime::currentMSecsSinceEpoch());

}

/**
 * Disconnect the connection.
 *
 * @return True if connection has been disconnected, false if connection
 * couldn't be disconnected.
 **/
bool MAVLinkSimulationLink::_disconnect(void)
{

    if(isConnected())
    {
        //        timer->stop();

        _isConnected = false;

        emit disconnected();

        //exit();
    }

    return true;
}

/**
 * Connect the link.
 *
 * @return True if connection has been established, false if connection
 * couldn't be established.
 **/
bool MAVLinkSimulationLink::_connect(void)
{
    _isConnected = true;
    emit connected();

    start(LowPriority);
    MAVLinkSimulationMAV* mav1 = new MAVLinkSimulationMAV(this, 1, 37.480391, -122.282883);
    Q_UNUSED(mav1);
//    MAVLinkSimulationMAV* mav2 = new MAVLinkSimulationMAV(this, 2, 47.375, 8.548, 1);
//    Q_UNUSED(mav2);
    //    timer->start(rate);
    return true;
}

/**
 * Check if connection is active.
 *
 * @return True if link is connected, false otherwise.
 **/
bool MAVLinkSimulationLink::isConnected() const
{
    return _isConnected;
}

int MAVLinkSimulationLink::getId() const
{
    return id;
}

QString MAVLinkSimulationLink::getName() const
{
    return name;
}

qint64 MAVLinkSimulationLink::getConnectionSpeed() const
{
    /* 100 Mbit is reasonable fast and sufficient for all embedded applications */
    return 100000000;
}

qint64 MAVLinkSimulationLink::getCurrentInDataRate() const
{
    return 0;
}

qint64 MAVLinkSimulationLink::getCurrentOutDataRate() const
{
    return 0;
}
