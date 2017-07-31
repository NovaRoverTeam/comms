/*
  comms.cpp

  Enables the use of roslibjs to communicate via service calls (in JSON) with a ROS network remotely over radio using the mavlink protocol.

  Author: Benjamin Steer    
*/

#include "ros/ros.h"
#include <iostream>
#include <vector>
#include <numeric>
#include <unistd.h>

#include <boost/thread.hpp>         // Handles multi-threading

#include "simpleweb/client_ws.hpp"  // WebSocket client library

#include "c_uart_interface_example/serial_port.h" // serial uart

#include <mainframe/DriveCommand.h>

using namespace std;
typedef SimpleWeb::SocketClient<SimpleWeb::WS> WsClient;

const int rb_port = 9090; // websocket port num

int platform; // 0 is GCS, 1 is vehicle

const int payload_len = 16;

// Declare websocket client
WsClient client("localhost:" + to_string(rb_port));
Serial_Port serial; // Declare serial port

ros::ServiceServer driveService;

const int max_msg_size = 16; // Max # frags any msg can split into (arbitrary)
const int max_str_size = payload_len - 3; // Max size of any msg fragment (96 - 3 byte header)

// Initialise message buffer
vector<vector<string> > buf(256, vector<string>(max_msg_size, ""));

// Each int describes how many frags of a msg have been received
int buf_cnt[256] = {0};
uint8_t msg_id = 0;

int time_cnt = 0; // Number of seconds since heartbeat

void send_mav_msg(string json_str)
/* Sends JSON message over mavlink. */
{
  json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces from JSON string.

  int total_size = json_str.length();
  uint8_t size = total_size / max_str_size; // Int div of msg size

  int mod = total_size % max_str_size; // Size of leftover msg
  if (mod != 0) size++; 

  //cout << endl << "Splitting: " << json_str << endl;
  //cout << "msg_id is: " << (int)msg_id << endl;

  for (uint8_t i = 0; i < size; i++) // Split message
  {
    int frame_size = max_str_size; // Size of string portion
    if (i == size-1) frame_size = mod; // Leftover
    
    // Grab portion of string for fragment
    string frag = json_str.substr(i*max_str_size, frame_size);
    const uint8_t *json_frag = reinterpret_cast<const uint8_t*>(frag.c_str()); // Convert string to uint8_t*

    //cout << "msg frag: " << frag << endl;

    uint8_t json_uint[payload_len] = {(uint8_t) ' '};
    json_uint[0] = msg_id;
    json_uint[1] = i;    // describes which fragment this is
    json_uint[2] = size;
    memcpy(&json_uint[3], json_frag, frame_size);

    mavlink_message_t json_msg;

    mavlink_msg_data16_pack(32, 1,               // sysid compid
                            &json_msg,              // msg
                            MAVLINK_TYPE_UINT8_T,   // type
                            sizeof(json_str),       // len of data
                            json_uint               // data to pack
                           );

/*
    mavlink_msg_heartbeat_pack(32, 0, &json_msg, MAV_TYPE_FIXED_WING, MAV_AUTOPILOT_GENERIC, MAV_MODE_PREFLIGHT, 0, MAV_STATE_STANDBY);
*/
    
    //cout << "before serial write" << endl;

    serial.write_message(json_msg);

    //cout << "after serial write" << endl;
  }  

  msg_id++; // Increment unique msg identifier
}

void forward_json(string json_str)
/* Forwards a JSON string over a websocket. */
{
  json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces

  json_str.erase(remove(json_str.begin(), json_str.end(), 0), json_str.end()); // Remove nulls

  auto send_stream = make_shared<WsClient::SendStream>();
  *send_stream << json_str;

  cout << "before websocket client send" << endl;

  client.send(send_stream);

  cout << "sending msg to bridge" << endl;
  cout << "msg is: " << json_str << endl;
}

void ws_thread()
/* Thread to run either websocket server or client. */
{
  //cout << "WebSocket CLIENT started!\n";

  client.on_message = [&client](shared_ptr<WsClient::Message> message) 
  {
    auto message_str = message->string();  // Grab incoming msg as string
      
    cout << "Client: Msg received: \"" << message_str << "\"" << endl;

    //send_mav_msg(message_str); // Send JSON via mavlink
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

//cout << "before serial read" << endl;
		bool success = serial.read_message(msg); // Read message
//cout << "after serial read" << endl;

		if(success)
    {
      switch(msg.msgid)
			{
        case MAVLINK_MSG_ID_DATA16:
          {
            //cout << endl << "Received a data16." << endl;
      
            uint8_t data[max_str_size];
            mavlink_msg_data16_get_data(&msg, data);

            ostringstream convert; // Convert uint8_t[] to string
            for (int a = 3; a < payload_len; a++) convert << data[a];
            string json_str = convert.str();

            // POPULATE BUFFER upon receiving mav msg
            uint8_t id   = data[0];
            uint8_t num  = data[1];
            uint8_t size = data[2];   

            cout << "id " << (int)id << endl;
            cout << "num " << (int)num << endl;
            cout << "size " << (int)size << endl;
      
            buf[id][num] = json_str; // Add string to buffer
            buf_cnt[id]++;
            cout << "buf_cnt " << buf_cnt[id] << endl;

            if (buf_cnt[id] == size) // If msg complete
            {
              cout << "last json str: " << json_str << endl;

              // Concat fragments
              string complete_msg = accumulate(buf[id].begin(), buf[id].end(), string(""));

              forward_json(complete_msg); // Send string over websocket

              buf_cnt[id] = 0; // Reset buffer entry
              buf[id] = vector<string>(max_msg_size, "");
            }
          }
          break;
        case MAVLINK_MSG_ID_HEARTBEAT:
          {
             forward_json("{\"op\":\"call_service\",\"id\":\"call_service:/CamCapture:2\",\"service\":\"/CamCapture\",\"args\":{\"data\":true}}");
          }
          break;
        default:
          cout << "not a data16??" << endl;
				  break;
      }
    }  

    //boost::this_thread::sleep(boost::posix_time::milliseconds(50));
  }
}

bool drive_cb(mainframe::DriveCommand::Request  &req,
         mainframe::DriveCommand::Response &res)
{
  int f_wheel_l = req.f_wheel_l; // Grab wheel PWM values
  int f_wheel_r = req.f_wheel_r;
  int b_wheel_l = req.b_wheel_l;
  int b_wheel_r = req.b_wheel_r;

  return true;
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

    boost::thread ws_t{ws_thread};
    if (platform == 1) boost::thread mav_t{mav_thread};
    
    ros::Rate loop_rate(1); // To send heartbeat once per second  

    int timeout = 3; // Num of secs til timeout with no hbeat

    if (platform == 0) 
    {
      driveService = n.advertiseService("DriveCommand", drive_cb);
    }

    cout << "before ros loop" << endl;

    while (ros::ok())
    {
      if ( serial.status != 1 ) // SERIAL_PORT_OPEN
	    {
		    fprintf(stderr,"ERROR: serial port not open\n");
		    throw 1;
      }

      if (platform == 0)
      {
        cout << "sending mav msg" << endl;
        send_mav_msg("{\"op\":\"call_service\",\"id\":\"call_service:/CamCapture:2\",\"service\":\"/CamCapture\",\"args\":{\"data\":true}}");

        //send_mav_msg("g");
      }
      
      ros::spinOnce();
      loop_rate.sleep();
    }

    cout << "after ros loop" << endl;

    //if (platform == 1) mav_t.join();
    ws_t.join(); 
    serial.stop();
  }
  else 
  {
    cout << arg_err << endl;
  }

  return 0;
}
