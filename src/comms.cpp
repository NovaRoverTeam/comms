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

#include "json.hpp"

#define LOOP_HZ 10

#define ROVER 1 // Platforms
#define BASE 0

using json = nlohmann::json; // for convenience
using namespace std;
typedef SimpleWeb::SocketServer<SimpleWeb::WS> WsServer;
typedef SimpleWeb::SocketClient<SimpleWeb::WS> WsClient;

boost::thread* ws_t_con; // Empty threads for global use
boost::thread* mav_t_con;

const int rb_port = 9090; // websocket port num

Serial_Port serial; // Declare serial port 
int platform; // 0 is GCS, 1 is vehicle

WsServer server; // Declare websocket server and client
WsClient client("localhost:" + to_string(rb_port));
shared_ptr<WsServer::Connection> con; // Stores server connection

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

string PREV_STATE = "STANDBY"; // Keep track of previous STATE parameter
bool waitingForGlen = false;


// *****************************************************************************
//    ROS Setup
// *****************************************************************************
ros::NodeHandle* n;

ros::Subscriber drivecmd_sub; // Declare ROS subscriber for drive commands
ros::Subscriber hbeat_sub;

ros::Publisher hbeat_pub;
ros::Publisher gps_pub;
ros::Publisher retrieve_pub;

ros::ServiceClient calc_route_client;
ros::ServiceServer calc_route_srv;
ros::ServiceServer start_auto_srv;
ros::ServiceServer set_dest_srv;

#include <std_msgs/Empty.h>
#include <rover/DriveCmd.h>
#include <rover/RedCmd.h>
#include <rover/Retrieve.h>
#include <gps/Gps.h>
#include <autopilot/calc_route.h>
#include <autopilot/grid_size.h>
#include <autopilot/set_dest.h>

autopilot::calc_route::Response* route_res; // Store and set response

#define GLEN_TIMEOUT 180 // Allowable time for Glen, in secs


// *****************************************************************************
// Sends mavlink heartbeat to test connection with other comms instance.
// *****************************************************************************
void send_mav_hbeat()
/* Sends mavlink heartbeat to test connection with other comms instance. */
{
  ROS_INFO("Sending mav hbeat");
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
  bool chk_sub = (j["op"].dump() == "subscribe");
  bool chk_srv = ((j["op"].dump() == "call_service") 
                        || (j["op"] == "service_response"));

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
    //ack_mtx.lock(); // Take mutex

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

    //ack_mtx.unlock(); // Release mutex
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

    ROS_INFO_STREAM("Off da mav goes whee");
    serial.write_message(json_msg);
    ROS_INFO_STREAM("it went off yall");
  }  

  msg_id++; // Increment unique msg identifier
}


// *****************************************************************************
// Subscribe to functions on the rover.
// *****************************************************************************
void subskrib()
{
  json j; // Create an empty json object

  // ROS info
  j["op"] = "subscribe";
  j["topic"] = "/gps/gps_data";
  j["type"] = "gps/Gps";
  j["queue_length"] = 0;
  j["id"] =   j["op"].dump()      + ":" 
            + j["topic"].dump() + ":" 
            + to_string(msg_id);

  string json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Send it off

  ROS_INFO_STREAM("now to sub to retrieve");

  // ROS info
  j["op"] = "subscribe";
  j["topic"] = "/retrieve";

  json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Send it off 
}


// *****************************************************************************
// Forwards a JSON string over a websocket.
// *****************************************************************************
void forward_json(string json_str)
{
  //json_str.erase(remove(json_str.begin(), json_str.end(), ' '), json_str.end()); // Remove whitespaces

  json_str.erase(remove(json_str.begin(), json_str.end(), 0), json_str.end()); // Remove nulls

  // Send received JSON to recipient via WebSocket
  if (platform == BASE && con != 0) 
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


//--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--
// Start_Auto:
//    Engages autonomous mode.
//--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--
bool Start_Auto(autopilot::calc_route::Request  &req,
                autopilot::calc_route::Response &res)
{   
  json j; // Create an empty json object

  // ROS info
  j["op"] = "call_service";
  j["service"] = "/Start_Auto";

  if (req.latlng == true)
  {
    j["args"]["latlng"] = true;
    j["args"]["destination"]["latitude"] = req.destination.latitude;
    j["args"]["destination"]["longitude"] = req.destination.longitude;
  }
  else
  {
    j["args"]["latlng"] = false;
    j["args"]["bearing"] = req.bearing;
    j["args"]["distance"] = req.distance;
  }

  string json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Send it off

  return true;
}


//--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--
// Calc_Route_set_response:
//    Sets the service response from the original call. Somehow. 
//    Apparently. Parses the JSON.
//--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--
void Calc_Route_set_response (json& j)
{
  json j_route = j["values"]["route"];

  vector<gps::Gps> gps_route;

  // iterate the array
  for (json::iterator it = j_route.begin(); it != j_route.end(); ++it) 
  {
    gps::Gps gps_msg; // Grab each gps coordinate on route

    gps_msg.latitude  = stod((*it)["latitude"].dump());
    gps_msg.longitude = stod((*it)["longitude"].dump());

    gps_route.push_back(gps_msg);
  }

  route_res->route = gps_route; // Set the GPS route
  waitingForGlen = false; // Stop waiting for glen and let the service go
}


//--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--
// Calc_Route_server:
//    Calculates route from Glen.
//--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--
bool Calc_Route_server (autopilot::calc_route::Request  &req,
                        autopilot::calc_route::Response &res)
{
  route_res = &res; // Store address of service response

  json j; // Create an empty json object

  // ROS info
  j["op"] = "call_service";
  j["service"] = "/Calc_Route";

  if (req.latlng == true)
  {
    j["args"]["latlng"] = true;
    j["args"]["destination"]["latitude"] = req.destination.latitude;
    j["args"]["destination"]["longitude"] = req.destination.longitude;
  }
  else
  {
    j["args"]["latlng"] = false;
    j["args"]["bearing"] = req.bearing;
    j["args"]["distance"] = req.distance;
  }

  string json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Send it off

  waitingForGlen = true; // Wait until response is set

  ros::Rate loop_rate(LOOP_HZ);
  ros::WallTime start_, now_;
  start_ = ros::WallTime::now();

  while(waitingForGlen) 
  {
    now_  = ros::WallTime::now(); // Update current time

    double elapsed_ = (now_ - start_).toSec();
    if (elapsed_ > GLEN_TIMEOUT) waitingForGlen = false;

    ros::spinOnce();
    loop_rate.sleep();
  }

  return true;
}


//--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--
// Calc_Route_client:
//    Calculates route from Glen.
//--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--
void Calc_Route_client(json& j)
{   
  autopilot::calc_route srv; // Create service message

  bool latlng = stoi(j["args"]["latlng"].dump()); 

  if (latlng)
  {
    gps::Gps gps_msg;
    gps_msg.latitude 
      = stod(j["args"]["destination"]["latitude"].dump());
    gps_msg.longitude 
      = stod(j["args"]["destination"]["longitude"].dump());

    srv.request.latlng = true;
    srv.request.destination = gps_msg;
  }
  else
  {
    srv.request.latlng = false;
    srv.request.distance 
      = stod(j["args"]["distance"].dump());
    srv.request.bearing 
      = stod(j["args"]["bearing"].dump());
  }

  bool success = calc_route_client.call(srv); // Call the service

  if (success)
  {
    ROS_INFO_STREAM("Service call to Calc_Route was a success!");

    json j; // Create an empty json object
    json j_route; // JSON object for the route of GPS coordinates

    // ROS info
    j["op"] = "service_response";
    j["service"] = "/Calc_Route";
    j["result"] = "true";

    vector<gps::Gps> route = srv.response.route;
    int n_wps = route.size();

    // Populate the latlng route list from the Gps route list
    for (int i=0; i<n_wps; i++)
    {
      gps::Gps coord = route[i];
      json j_coord; // JSON object for a particular GPS coordinate

      j_coord["latitude"] = coord.latitude;
      j_coord["longitude"] = coord.longitude;

      j_route.push_back(j_coord);
    }

    j["values"]["route"] = j_route;

    string json_str = j.dump(); // Convert JSON object to string

    ROS_INFO_STREAM("Route service JSON msg was: " << json_str);

    send_mav_msg(json_str); // Send it off
  }
  else ROS_INFO_STREAM("Service call to Calc_Route failed!");
}


//--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--
// Set_Dest:
//    Set the destination for the interface to display.
//--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--**--..--
bool Set_Dest(autopilot::set_dest::Request  &req,
              autopilot::set_dest::Response &res)
{   
  json j; // Create an empty json object

  // ROS info
  j["op"] = "call_service";
  j["service"] = "/Set_Dest";

  j["args"]["latitude"] = req.destination.latitude;
  j["args"]["longitude"] = req.destination.longitude;

  string json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Send it off

  return true;
}


// *****************************************************************************
// Publisher for GPS topic
// *****************************************************************************
void gps_pub_cb(json& j)
{
  gps::Gps msg;

  msg.latitude  = stod(j["msg"]["latitude"].dump());
  msg.longitude = stod(j["msg"]["longitude"].dump());

  gps_pub.publish(msg);
}


// *****************************************************************************
// Publisher for retrieve topic
// *****************************************************************************
void retrieve_pub_cb(json& j)
{
  rover::Retrieve msg;

  msg.distance  = stod(j["msg"]["distance"].dump());
  msg.bearing = stod(j["msg"]["bearing"].dump());

  retrieve_pub.publish(msg);
}


// *****************************************************************************
// Heartbeat callback
// *****************************************************************************
void hbeat_cb(const std_msgs::Empty::ConstPtr& msg)
{
  send_mav_hbeat(); // Send a heartbeat over the radio
}


// *****************************************************************************
// Callback for drive command data
// *****************************************************************************
void cmd_data_cb(const rover::DriveCmd::ConstPtr& msg)
{
  json j; // Create an empty json object

  // ROS info
  j["op"] = "publish";
  j["topic"] = "/mainframe/cmd_data";
  //j["id"] =   j["op"].dump()      + ":" 
  //          + j["topic"].dump() + ":" 
  //          + to_string(msg_id);

  // Message contents
  j["msg"]["acc"] = to_string(msg->acc);
  j["msg"]["steer"] = to_string(msg->steer);

  string json_str = j.dump(); // Convert JSON object to string

  ROS_INFO_STREAM(json_str << endl);

  send_mav_msg(json_str); // Begin mavlink sending process
}


// *****************************************************************************
// Callback for Redback command data
// *****************************************************************************
void red_cmd_data_cb(const rover::RedCmd::ConstPtr& msg)
{
  json j; // Create an empty json object

  // ROS info
  j["op"] = "publish";
  j["topic"] = "/mainframe/red_cmd_data";
  //j["id"] =   j["op"].dump()      + ":" 
  //          + j["topic"].dump() + ":" 
  //          + to_string(msg_id);

  // Message contents
  j["msg"]["kill"] = to_string(msg->kill);
  j["msg"]["drillSpd"] = to_string(msg->drillSpd);
  j["msg"]["stepperPos"] = to_string(msg->stepperPos);
  j["msg"]["actuatorSpeed"] = to_string(msg->actuatorSpeed);
  j["msg"]["sensorSpeed"] = to_string(msg->sensorSpeed);

  string json_str = j.dump(); // Convert JSON object to string

  ROS_INFO_STREAM(json_str << endl);

  send_mav_msg(json_str); // Begin mavlink sending process
}


// *****************************************************************************
//    Set the STATE parameter on the other end
// *****************************************************************************
void set_state(string STATE)
{
  json j; // Create an empty json object

  j["op"] = "param";
  j["STATE"] = STATE;

  string json_str = j.dump(); // Convert JSON object to string
  send_mav_msg(json_str); // Begin mavlink sending process
}


// *****************************************************************************
// Thread to handle incoming mavlink messages.
// *****************************************************************************
void mav_thread()
{
  while (true)
  {
    mavlink_message_t msg;
    bool success = serial.read_message(msg); // Read message

    if(success)
    {
      ROS_INFO_STREAM("Received mav msg");
      switch(msg.msgid)
	    {
        case MAVLINK_MSG_ID_DATA96: // ROS MSG/SERVICE/PARAMETER COMING THROUGH
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

                  ROS_INFO_STREAM("defragmented, msg is: " << complete_msg);

                  try
                  {
                    auto j = json::parse(complete_msg);

                    // Check if we need to set a parameter
                    string op = j["op"].dump();
                    if (op == "param")
                    {
                      string STATE = j["STATE"].dump();
                      n->setParam("/STATE", STATE);
                    }

                    // Check if this is for rosbridge or a custom callback/pubber
                    if (platform == BASE)
                    {
                      // Look at message topics
                      string topic = j["topic"].dump();
                      if  (topic == "/gps/gps_data") gps_pub_cb(j);
                      else if (topic == "/retrieve") retrieve_pub_cb(j);       

                      // Look at services
                      string service = j["service"].dump();       
                      if (service == "/Calc_Route") Calc_Route_client(j);         
                    }
                    else // platform == ROVER
                    {
                      // Look at services
                      string service = j["service"].dump(); 
                      if (service == "/Calc_Route") Calc_Route_set_response(j); 
                      else forward_json(complete_msg); 
                    }

                     // Send string over

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

        case MAVLINK_MSG_ID_HEARTBEAT: // HEARTBEAT
          {
             // Only ROVER gets a heartbeat
             std_msgs::Empty msg;
             hbeat_pub.publish(msg);
          }
          break;

        case MAVLINK_MSG_ID_DATA16: // MESSAGE ACKNOWLEDGEMENT
          {
            uint8_t id = mavlink_msg_data16_get_type(&msg);

            //ack_mtx.lock(); // Take mutex TODO confirm this works?

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

              ROS_INFO_STREAM("Msg with id " << unsigned(id) << " acked.");
              ROS_INFO_STREAM("Resending list now contains " << ack_list.size() << " messages.");
            }
            else ROS_INFO_STREAM("No msg found in ack_list with id: " << unsigned(id));

            //ack_mtx.unlock(); // Release mutex TODO confirm this works?

          }
          break;

        default:
				  break;
      }
    }  

    boost::this_thread::sleep(boost::posix_time::milliseconds(10));
  }
}


// *****************************************************************************
// Override the default ros sigint handler to shut down other threads. 
// *****************************************************************************
void mySigintHandler(int sig)
{
  ws_t_con->interrupt();
  mav_t_con->interrupt();

  ws_t_con->join(); 
  mav_t_con->join();

  // All the default sigint handler does is call shutdown()
  ros::shutdown();
}


// *****************************************************************************
// Classic main function!
// *****************************************************************************
int main(int argc, char ** argv)
{
  ros::init(argc, argv, "comms");
  n = new ros::NodeHandle();
  ros::Rate loop_rate(LOOP_HZ);

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
    
    // If rover, set up heartbeat publisher and mav message thread
    if (platform == ROVER) 
    {
      hbeat_pub = n->advertise<std_msgs::Empty>("/hbeat", 1);

      calc_route_srv
        = n->advertiseService("/Calc_Route", Calc_Route_server);

      boost::thread ws_t{ws_thread};
      ws_t_con = &ws_t;
    }
    else // platform == BASE
    {
      start_auto_srv
        = n->advertiseService("/Start_Auto", Start_Auto);
      set_dest_srv 
        = n->advertiseService("/Set_Dest", Set_Dest);
      calc_route_client 
        = n->serviceClient<autopilot::calc_route>("/Calc_Route");

      drivecmd_sub = n->subscribe("/mainframe/cmd_data", 1, cmd_data_cb);	
      hbeat_sub = n->subscribe("/hbeat", 1, hbeat_cb);
      gps_pub = n->advertise<gps::Gps>("/gps/gps_data", 1);	
      retrieve_pub = n->advertise<rover::Retrieve>("/retrieve", 1);

      subskrib();	
    }
    
    boost::thread mav_t{mav_thread};    
    mav_t_con = &mav_t;

    while (ros::ok())
    {      
      string STATE; n->getParam("/STATE", STATE);
      if (PREV_STATE != STATE) set_state(STATE); // if change, set param 
      PREV_STATE = STATE; // Update prev state

      // Send acks
      //ack_mtx.lock(); // Take mutex

      for (int i=0; i < ack_list.size(); i++)
      {
        if (ack_secs[i] == 0)
        {
          send_mav_msg(ack_list[i], ack_id[i]); // Send ack
          ack_secs[i] = ack_period;
        }
        else ack_secs[i] -= 1;
      }

      //ack_mtx.unlock(); // Release mutex

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

