/*
  comms.cpp

  Enables the forwarding of ROS messages/services remotely over radio using the mavlink protocol.

  Author: Benjamin Steer    
*/

#include "ros/ros.h"
#include <ros/console.h>

#include <iostream>
#include <sstream>
#include <vector>
#include <numeric>
#include <unistd.h>
#include <stdlib.h>
#include <algorithm>

#include <boost/thread.hpp>         // Handles multi-threading

#include "simpleweb/client_ws.hpp"  // WebSocket client library
#include "simpleweb/server_ws.hpp" // WebSocket server library
#include "c_uart_serial/serial_port.h" // serial uart

#include <std_msgs/Empty.h>
#include <rover/DriveCmd.h>

#include "json.hpp"


using json = nlohmann::json; // for convenience
using namespace std;
typedef SimpleWeb::SocketServer<SimpleWeb::WS> WsServer;
typedef SimpleWeb::SocketClient<SimpleWeb::WS> WsClient;

boost::thread* ws_t_con; // Empty threads for global use
boost::thread* mav_t_con;

const int ws_port = 8091;    // websocket port num
const int rb_port = 9090;    // websocket port num

shared_ptr<mlink> mav_link;
int platform; // 0 is GCS, 1 is vehicle

WsServer server; // Declare websocket server and client
WsClient client("localhost:" + to_string(rb_port));
shared_ptr<WsServer::Connection> con; // Stores server connection

Serial_Port serial; // Declare serial port 


const int max_msg_size = 16; // Max # frags any msg can split into (arbitrary)
const int max_str_size = 93; // Max size of any msg fragment (96 - 3 byte header)

const int sysid_gnd = 14;
const int sysid_air = 244;
int sysid = 0; // Initialise sysid, assign on setup

// Initialise message buffer
vector<vector<string> > buf(256, vector<string>(max_msg_size, ""));

// Each int describes how many frags of a msg have been received
int buf_cnt[256] = {0};
//bool buf_frags[256][max_msg_size] = {0};
uint8_t msg_id = 0;

vector<string> ack_list; // Vector of high priority messages
vector<uint8_t> ack_id; // Vector of high priority msg ids
vector<uint8_t> ack_secs; // Vector of time until resending msg
uint8_t ack_period = 2; // Seconds to wait before resending msg
boost::mutex ack_mtx; // Mutex for ack vectors
int max_ack_list_size = 40; // Only hold this many msgs for resending

int time_cnt = 0; // Number of seconds since heartbeat


// *****************************************************************************
//    ROS Setup
// *****************************************************************************
ros::Subscriber drivecmd_sub; // Declare ROS subscriber for drive commands
ros::Publisher hbeat_pub;


// *****************************************************************************
// Sends mavlink heartbeat to test connection with other comms instance.
// *****************************************************************************
void send_mav_hbeat()
/* Sends mavlink heartbeat to test connection with other comms instance. */
{
  mavlink_message_t hbeat_msg;

  mavlink_msg_heartbeat_pack(0, 0, &hbeat_msg, MAV_TYPE_FIXED_WING, MAV_AUTOPILOT_GENERIC, MAV_MODE_PREFLIGHT, 0, MAV_STATE_STANDBY);

  serial.write_message(hbeat_msg); // Send it off
}



// *****************************************************************************
// Sends mavlink acknowledgement msg to confirm message arrival.
// *****************************************************************************
void send_mav_ack(uint8_t id)
{
  mavlink_message_t ack_msg;

  // Using data16 because why not, just need to send a single uint8_t
  // Being sneaky and using its "type" field for id
  mavlink_msg_data16_pack(sysid, 0, &ack_msg, id, 0, 0);

  serial.write_message(ack_msg); // Send it off
}


// *****************************************************************************
// Returns true if the ROS msg is a priority topic.
// *****************************************************************************
bool is_priority(nlohmann::json& j)
{
  bool chk_sub = (j["op"] == "subscribe");
  bool chk_srv = ((j["op"] == "call_service") || (j["op"] == "service_response"));

  return (chk_sub || chk_srv);
}


// *****************************************************************************
// Sends JSON message over mavlink.
// *****************************************************************************
void send_mav_msg(string json_str, int force_id = -1)
{
  //json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces from JSON string.

  ROS_INFO_STREAM("Sending mav msg: " << json_str << endl);

  auto j = json::parse(json_str);

  if (is_priority(j) && force_id < 0)
  {
    ack_mtx.lock(); // Take mutex

    int n_acks = ack_list.size(); // Number of msgs seeking acknowledgements

    if (n_acks < max_ack_list_size) // Don't hold too many msgs to resend
    {
      // Add json message and message id to ack list
      ack_list.resize(n_acks+1, json_str);
      ack_id.resize(n_acks+1, msg_id);
      ack_secs.resize(n_acks+1, ack_period);

      ROS_INFO_STREAM("Assigning msg " << unsigned(msg_id) << " for resending.");
      ROS_INFO_STREAM("Resending list now contains " << n_acks+1 << " messages.");
    }

    ack_mtx.unlock(); // Release mutex
  }
  
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

    uint8_t json_uint[96] = {(uint8_t) ' '};

    if (force_id < 0) json_uint[0] = msg_id;
    else              json_uint[0] = force_id;

    json_uint[1] = i;    // describes which fragment this is
    json_uint[2] = size;
    memcpy(&json_uint[3], json_frag, frame_size);

    mavlink_message_t json_msg;

    mavlink_msg_data96_pack(sysid, 1,               // sysid compid
                            &json_msg,              // msg
                            MAVLINK_TYPE_UINT8_T,   // type
                            sizeof(json_str),       // len of data
                            json_uint               // data to pack
                           );

    serial.write_message(json_msg);
  }  

  msg_id++; // Increment unique msg identifier
}


// *****************************************************************************
// Forwards a JSON string over a websocket.
// *****************************************************************************
void forward_json(string json_str)
{
  //json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces

  json_str.erase(remove(json_str.begin(), json_str.end(), 0), json_str.end()); // Remove nulls

  // Send received JSON to recipient via WebSocket
  if (platform == 0 && con != 0) 
  { // GCS
    auto send_stream = make_shared<WsServer::SendStream>();
    *send_stream << json_str;

    server.send(con, send_stream, \
      [](const boost::system::error_code& ec) { });

    ROS_INFO_STREAM("sending lil msg to interface" << endl);
    ROS_INFO_STREAM("msg is: " << json_str << endl);
  }
  else                            
  { // Vehicle   
    auto send_stream = make_shared<WsClient::SendStream>();
    *send_stream << json_str;

    client.send(send_stream);

    ROS_INFO_STREAM("sending lil msg to bridge" << endl);
    ROS_INFO_STREAM("msg is: " << json_str << endl);
  }
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


// *****************************************************************************
// Thread to handle incoming mavlink messages.
// *****************************************************************************
void mav_thread()
{
  while (true)
  {
    mavlink_message_t msg;
    while(mav_link->qReadIncoming(&msg)) 
    {
      switch(msg.msgid)
	  {
        case MAVLINK_MSG_ID_DATA96:
          {
            if (msg.sysid == sysid_gnd || msg.sysid == sysid_air)
            {
              ROS_DEBUG_STREAM(endl << "Got a cheeky data96!" << endl);
        
              uint8_t data[max_str_size];
              mavlink_msg_data96_get_data(&msg, data);

              ostringstream convert; // Convert uint8_t[] to string
              for (int a = 3; a < 96; a++) convert << data[a];
              string json_str = convert.str();

              // POPULATE BUFFER upon receiving mav msg
              uint8_t id   = data[0];
              uint8_t num  = data[1];
              uint8_t size = data[2];

              ROS_DEBUG_STREAM("id " << (int)id << endl);
              ROS_DEBUG_STREAM("num " << (int)num << endl);
              ROS_DEBUG_STREAM("size " << (int)size << endl);

              // Check if msg fragment has already been received
              if (buf[id][num] != json_str)
              {
                ROS_DEBUG_STREAM("New msg fragment found. Adding to collection :)");
  
                // Only increment counter if buffer fragment is 100% empty
                if (buf[id][num] == "") buf_cnt[id]++;

                buf[id][num] = json_str; // Add string to buffer                

                if (buf_cnt[id] == size) // If msg complete, assemble and forward
                {
                  // Concat fragments
                  string complete_msg = accumulate(buf[id].begin(), buf[id].end(), string(""));

                  try
                  {
                    auto j = json::parse(complete_msg);

                    forward_json(complete_msg); // Send string over

                    buf_cnt[id] = 0; // Reset buffer entry
                    buf[id] = vector<string>(max_msg_size, "");

                    ROS_DEBUG_STREAM("Accum buf entry reset for msg " << unsigned(id));                                

                    // Check if msg is priority topic
                    if (is_priority(j))
                    {
                      send_mav_ack(id); // Send ack in response to msg arrival
                    }
                  }
                  catch (nlohmann::detail::parse_error e)
                  {
                    ROS_INFO_STREAM("JSON parsing exception thrown. Clearing buffer entry.");

                    buf_cnt[id] = 0; // Reset buffer entry
                    buf[id] = vector<string>(max_msg_size, "");
                  }  
                }
              }
              else ROS_INFO_STREAM("Msg fragment already received. Ignoring.");
              
            }
          }
          break;
        case MAVLINK_MSG_ID_HEARTBEAT:
          {
             // If GCS receives heartbeat, tell interface
             if (platform == 0 && con!= 0) 
             {
               forward_json("{\"topic\":\"/mrbridge_hbeat\",\"msg\":{\"data\":true},\"op\":\"publish\"}"); 
               time_cnt = 0; // Reset number of heartbeats
             }
             else if (platform == 1)
             { 
               send_mav_heartbeat(); // If vehicle receives heartbeat, send one back
             }
          }
          break;
        case MAVLINK_MSG_ID_DATA16:
          {
            uint8_t id = mavlink_msg_data16_get_type(&msg);

            //ack_mtx.lock(); // Take mutex

            // Iterators to locate and delete msg from ack lists
            std::vector<uint8_t>::iterator it;
            it = find(ack_id.begin(), ack_id.end(), id);
            int index = it - ack_id.begin(); // Index of element to be removed          

            if (it != ack_id.end())
            {
              // Clear ack from lists so we don't resend this msg any more.
              ack_list.erase(ack_list.begin() + index);
              ack_id.erase(ack_id.begin() + index);
              ack_secs.erase(ack_secs.begin() + index);

              ROS_INFO_STREAM("Msg with id " << unsigned(id) << " acked."); // TODO

              ROS_INFO_STREAM("Resending list now contains " << ack_list.size() << " messages.");
            }
            else ROS_INFO_STREAM("No msg found in ack_list with id: " << unsigned(id));

            //ack_mtx.unlock(); // Release mutex

          }
          break;
        default:
				  break;
      }
    }  

    boost::this_thread::sleep(boost::posix_time::milliseconds(10));
  }
}


void cmd_data_cb(const rover::DriveCmd::ConstPtr& msg)
{
  string json_str = "{\"op\":\"publish\",\"id\":\"publish:/mainframe/cmd_data:" + \
                    to_string(msg_id) + \
                    "\",\"topic\":\"/mainframe/cmd_data\",\"msg\":{\"acc\":" + \
                    to_string(msg->acc) + ",\"steer\":" + \
                    to_string(msg->steer) + "}}";

  ROS_INFO_STREAM(json_str << endl);

  send_mav_msg(json_str);
}

int main(int argc, char ** argv)
{
  ros::init(argc, argv, "comms");
  ros::NodeHandle n;

  ros::Subscriber drivecmd_sub;

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

      hbeat_pub = n.advertise<std_msgs::Empty>("hbeat", 1);

      boost::thread mav_t{mav_thread};
      boost::thread ws_t{ws_thread};
    }
    else
    {      
      rate = 4; // Loop used to get data from mainframe

      drivecmd_sub = n.subscribe("/mainframe/cmd_data", 5, cmd_data_cb);	
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

