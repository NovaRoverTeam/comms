/*
  comms.cpp

  Enables the forwarding of ROS messages/services remotely over radio using the mavlink protocol.

  Author: Benjamin Steer    
*/

#include "ros/ros.h"
#include <ros/console.h>

#include <iostream>
#include <vector>
#include <numeric>
#include <unistd.h>

#include <boost/thread.hpp>         // Handles multi-threading

#include "simpleweb/client_ws.hpp"  // WebSocket client library
#include "c_uart_serial/serial_port.h" // serial uart

#include <std_msgs/Empty.h>
#include <rover/DriveCmd.h>

using namespace std;
typedef SimpleWeb::SocketClient<SimpleWeb::WS> WsClient;

const int rb_port = 9090; // websocket port num

int platform; // 0 is base station, 1 is rover

const int payload_len = 96; 

// Declare websocket client 
WsClient client("localhost:" + to_string(rb_port)); 
Serial_Port serial; // Declare serial port 

ros::Subscriber drivecmd_sub; // Declare ROS subscriber for drive commands
ros::Publisher hbeat_pub;

const int max_msg_size = 16; // Max # frags any msg can split into (arbitrary)
const int max_str_size = payload_len - 3; // Max size of any msg fragment (96 - 3 byte header)

// Initialise message buffer
vector<vector<string> > buf(256, vector<string>(max_msg_size, ""));

// Each int describes how many frags of a msg have been received
int buf_cnt[256] = {0};
uint8_t msg_id = 0;


void send_mav_hbeat()
/* Sends mavlink heartbeat to test connection with other comms instance. */
{
  mavlink_message_t hbeat_msg;

  mavlink_msg_heartbeat_pack(0, 0, &hbeat_msg, MAV_TYPE_FIXED_WING, MAV_AUTOPILOT_GENERIC, MAV_MODE_PREFLIGHT, 0, MAV_STATE_STANDBY);

  serial.write_message(hbeat_msg); // Send it off
}

void send_mav_msg(string json_str)
/* Sends JSON message over mavlink. */
{
  json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces from JSON string.

  int total_size = json_str.length();
  uint8_t size = total_size / max_str_size; // Int div of msg size

  int mod = total_size % max_str_size; // Size of leftover msg
  if (mod != 0) size++; 

  for (uint8_t i = 0; i < size; i++) // Split message
  {
    int frame_size = max_str_size; // Size of string portion
    if (i == size-1) frame_size = mod; // Leftover
    
    // Grab portion of string for fragment
    string frag = json_str.substr(i*max_str_size, frame_size);
    const uint8_t *json_frag = reinterpret_cast<const uint8_t*>(frag.c_str()); // Convert string to uint8_t*

    uint8_t json_uint[payload_len] = {(uint8_t) ' '};
    json_uint[0] = msg_id;
    json_uint[1] = i;    // describes which fragment this is
    json_uint[2] = size;
    memcpy(&json_uint[3], json_frag, frame_size);

    mavlink_message_t json_msg;

    mavlink_msg_data96_pack(32, 1,               // sysid compid
                            &json_msg,              // msg
                            MAVLINK_TYPE_UINT8_T,   // type
                            sizeof(json_str),       // len of data
                            json_uint               // data to pack
                           );

    serial.write_message(json_msg);
  }  

  msg_id++; // Increment unique msg identifier
}

void forward_json(string json_str)
/* Forwards a JSON string to rosbridge. */
{
  json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces

  json_str.erase(remove(json_str.begin(), json_str.end(), 0), json_str.end()); // Remove nulls

  auto send_stream = make_shared<WsClient::SendStream>();
  *send_stream << json_str;

  ROS_INFO_STREAM("Received JSON: " << json_str << endl);

  client.send(send_stream);
}

void ws_thread()
/* Thread to run websocket client. */
{
  client.on_message = [&client](shared_ptr<WsClient::Message> message) 
  {
    auto message_str = message->string();  // Grab incoming msg as string
      
    cout << "Client: Msg received: \"" << message_str << "\"" << endl;

    send_mav_msg(message_str); // Send JSON via mavlink
  };

  client.on_open=[&client]() {
    cout << "Client: Opened connection" << endl;
  };

  client.on_error = [&client](const boost::system::error_code& ec) 
  {
    boost::this_thread::sleep(boost::posix_time::milliseconds(3000));
    cout << "Client: Error: " << ec << ", error message: " << ec.message() << endl;
    cout << "Attempting to connect..." << endl;
    client.start();
  };

  client.start();
}

void mav_thread()
/* Thread to handle incoming mavlink messages. */
{
  while (true)
  {
    mavlink_message_t msg;

    bool success = serial.read_message(msg); // Read message

    if(success)
    {
      switch(msg.msgid)
			{
        case MAVLINK_MSG_ID_DATA96:
        {
          uint8_t data[max_str_size];
          mavlink_msg_data96_get_data(&msg, data);

	  ROS_INFO_STREAM("Got a cheeky data96" << endl);

          ostringstream convert; // Convert uint8_t[] to string
          for (int a = 3; a < payload_len; a++) convert << data[a];
          string json_str = convert.str();

          // POPULATE BUFFER upon receiving mav msg
          uint8_t id   = data[0];
          uint8_t num  = data[1];
          uint8_t size = data[2];   
    
          buf[id][num] = json_str; // Add string to buffer
          buf_cnt[id]++;

          if (buf_cnt[id] == size) // If msg complete
          {
            // Concat fragments
            string complete_msg = accumulate(buf[id].begin(), buf[id].end(), string(""));

            forward_json(complete_msg); // Send string to rosbridge

            buf_cnt[id] = 0; // Reset buffer entry
            buf[id] = vector<string>(max_msg_size, "");
          }
        } break;

        case MAVLINK_MSG_ID_HEARTBEAT:
        {
          // If rover receives heartbeat, tell underling
          if (platform == 1)
          { 
            std_msgs::Empty msg;
            hbeat_pub.publish(msg);
          }
        } break;

        default:
          cout << "not a data96??" << endl;
				  break;
      }
    }  

    //boost::this_thread::sleep(boost::posix_time::milliseconds(50));
  }
}

void cmd_data_cb(const rover::DriveCmd::ConstPtr& msg)
{
  string json_str = "{\"op\":\"publish\",\"id\":\"publish:/mainframe/cmd_data:" + \
                    to_string(msg_id) + \
                    "\",\"topic\":\"/mainframe/cmd_data\",\"msg\":{\"acc\":" + \
                    to_string(msg->acc) + ",\"steer\":" + \
                    to_string(msg->steer) + "}}";

  cout << json_str << endl;

  send_mav_msg(json_str);
}

int main(int argc, char ** argv)
{
  ros::init(argc, argv, "comms");
  ros::NodeHandle n;

  string arg_err = "First cmd line arg should be 0 for base station, 1 for rover. Second should specify serial device path, e.g. /dev/ttyUSB1\n";

  if (argc == 3)
  {  
    platform = stoi(argv[1]); // Grab cmd line arg for platform mode
    if ((platform < 0) || (platform > 1)) // If bad cmd arg, terminate 
    {
      cout << arg_err << endl;
      return 0;
    }

    char* dev = argv[2]; // serial device path

    int baud_rate = 57600;  // Baud rate for serial comms

    serial = Serial_Port(dev, baud_rate);
    serial.start();

    int rate = 0; // Rate for main ROS loop
    
    // If rover, set up heartbeat publisher and mav message thread
    if (platform == 1) 
    {
      rate = 2; // Loop only used to facilitate publishing heartbeats to underling

      hbeat_pub = n.advertise<std_msgs::Empty>("hbeat", 10);

      boost::thread mav_t{mav_thread};
      boost::thread ws_t{ws_thread};
    }
    else
    {      
      rate = 4; // Loop used to get data from mainframe

      ros::Subscriber drivecmd_sub = n.subscribe("/mainframe/cmd_data", 10, cmd_data_cb);	
    }

    ros::Rate loop_rate(rate);

    while (ros::ok())
    {      
      // If base station, send heartbeat to rover
      if (platform == 0) send_mav_hbeat();

      ros::spinOnce();
      loop_rate.sleep();
    }

    serial.stop();
  }
  else 
  {
    cout << arg_err << endl;
  }

  return 0;
}

