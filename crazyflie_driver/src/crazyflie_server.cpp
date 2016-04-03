#include "ros/ros.h"
#include <tf/transform_listener.h>

#include "crazyflie_driver/AddCrazyflie.h"
#include "crazyflie_driver/LogBlock.h"
#include "crazyflie_driver/GenericLogData.h"
#include "crazyflie_driver/UpdateParams.h"
#include "crazyflie_driver/UploadTrajectory.h"
#include "std_srvs/Empty.h"
#include "geometry_msgs/Twist.h"
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/Temperature.h"
#include "sensor_msgs/MagneticField.h"
#include "std_msgs/Float32.h"

//#include <regex>
#include <thread>
#include <mutex>

#include <crazyflie_cpp/Crazyflie.h>

constexpr double pi() { return std::atan(1)*4; }

double degToRad(double deg) {
    return deg / 180.0 * pi();
}

double radToDeg(double rad) {
    return rad * 180.0 / pi();
}

class CrazyflieROS
{
public:
  CrazyflieROS(
    const std::string& link_uri,
    const std::string& tf_prefix,
    const std::string& frame,
    const std::string& worldFrame,
    float roll_trim,
    float pitch_trim,
    bool enable_logging,
    bool enable_parameters,
    std::vector<crazyflie_driver::LogBlock>& log_blocks)
    : m_cf(link_uri)
    , m_tf_prefix(tf_prefix)
    , m_frame(frame)
    , m_worldFrame(worldFrame)
    , m_isEmergency(false)
    , m_roll_trim(roll_trim)
    , m_pitch_trim(pitch_trim)
    , m_enableLogging(enable_logging)
    , m_enableParameters(enable_parameters)
    , m_logBlocks(log_blocks)
    , m_serviceEmergency()
    , m_serviceUpdateParams()
    , m_serviceUploadTrajectory()
    , m_serviceStartTrajectory()
    , m_serviceTakeoff()
    , m_serviceLand()
    , m_subscribeCmdVel()
    , m_pubImu()
    , m_pubTemp()
    , m_pubMag()
    , m_pubPressure()
    , m_pubBattery()
    , m_pubRssi()
    , m_listener()
  {
    ros::NodeHandle n;
    m_serviceEmergency = n.advertiseService(tf_prefix + "/emergency", &CrazyflieROS::emergency, this);
    // m_serviceUpdateParams = n.advertiseService(tf_prefix + "/update_params", &CrazyflieROS::updateParams, this);
    m_serviceUploadTrajectory = n.advertiseService(tf_prefix + "/upload_trajectory", &CrazyflieROS::uploadTrajectory, this);
    m_serviceStartTrajectory = n.advertiseService(tf_prefix + "/start_trajectory", &CrazyflieROS::startTrajectory, this);
    m_serviceTakeoff = n.advertiseService(tf_prefix + "/takeoff", &CrazyflieROS::takeoff, this);
    m_serviceLand = n.advertiseService(tf_prefix + "/land", &CrazyflieROS::land, this);

    m_pubImu = n.advertise<sensor_msgs::Imu>(tf_prefix + "/imu", 10);
    m_pubTemp = n.advertise<sensor_msgs::Temperature>(tf_prefix + "/temperature", 10);
    m_pubMag = n.advertise<sensor_msgs::MagneticField>(tf_prefix + "/magnetic_field", 10);
    m_pubPressure = n.advertise<std_msgs::Float32>(tf_prefix + "/pressure", 10);
    m_pubBattery = n.advertise<std_msgs::Float32>(tf_prefix + "/battery", 10);
    m_pubRssi = n.advertise<std_msgs::Float32>(tf_prefix + "/rssi", 10);

    for (auto& logBlock : m_logBlocks)
    {
      m_pubLogDataGeneric.push_back(n.advertise<crazyflie_driver::GenericLogData>(tf_prefix + "/" + logBlock.topic_name, 10));
    }

    std::thread t(&CrazyflieROS::run, this);
    t.detach();
  }

private:
  struct logImu {
    float acc_x;
    float acc_y;
    float acc_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
  } __attribute__((packed));

  struct log2 {
    float mag_x;
    float mag_y;
    float mag_z;
    float baro_temp;
    float baro_pressure;
    float pm_vbat;
  } __attribute__((packed));

private:
  bool emergency(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    ROS_FATAL("Emergency requested!");
    m_isEmergency = true;

    return true;
  }

  template<class T, class U>
  void updateParam(uint8_t id, const std::string& ros_param) {
      U value;
      ros::param::get(ros_param, value);
      m_cf.setParam<T>(id, (T)value);
  }

  bool updateParams(
    crazyflie_driver::UpdateParams::Request& req,
    crazyflie_driver::UpdateParams::Response& res)
  {
    ROS_INFO("Update parameters");
    for (auto&& p : req.params) {
      std::string ros_param = "/" + m_tf_prefix + "/" + p;
      size_t pos = p.find("/");
      std::string group(p.begin(), p.begin() + pos);
      std::string name(p.begin() + pos + 1, p.end());

      auto entry = m_cf.getParamTocEntry(group, name);
      if (entry)
      {
        switch (entry->type) {
          case Crazyflie::ParamTypeUint8:
            updateParam<uint8_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt8:
            updateParam<int8_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeUint16:
            updateParam<uint16_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt16:
            updateParam<int16_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeUint32:
            updateParam<uint32_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt32:
            updateParam<int32_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeFloat:
            updateParam<float, float>(entry->id, ros_param);
            break;
        }
      }
      else {
        ROS_ERROR("Could not find param %s/%s", group.c_str(), name.c_str());
      }
    }
    return true;
  }

  bool uploadTrajectory(
    crazyflie_driver::UploadTrajectory::Request& req,
    crazyflie_driver::UploadTrajectory::Response& res)
  {
    ROS_INFO("Upload trajectory");

    m_cf.trajectoryReset();

    for (auto& p : req.points) {
      m_cf.trajectoryAdd(
        p.position.x, p.position.y, p.position.z,
        p.velocity.x, p.velocity.y, p.velocity.z,
        p.yaw,
        p.time_from_start.toSec() * 1000);
    }

    m_cf.trajectoryStart();

    return true;
  }

  bool startTrajectory(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    ROS_INFO("Start trajectory");

    m_cf.trajectoryStart();

    return true;
  }

  bool takeoff(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    ROS_INFO("Takeoff");

    tf::StampedTransform transform;
    m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);

    m_cf.trajectoryReset();
    m_cf.trajectoryAdd(
      transform.getOrigin().x(),
      transform.getOrigin().y(),
      transform.getOrigin().z(),
      0, 0, 0, 0, 0);
    m_cf.trajectoryAdd(
      transform.getOrigin().x(),
      transform.getOrigin().y(),
      0.5, //transform.getOrigin().z + 0.5,
      0, 0, 0, 0, 2 * 1000);
    m_cf.trajectoryStart();
    m_cf.setTrajectoryState(true);

    return true;
  }

  bool land(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    ROS_INFO("land");

    tf::StampedTransform transform;
    m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);

    m_cf.trajectoryReset();
    m_cf.trajectoryAdd(
      transform.getOrigin().x(),
      transform.getOrigin().y(),
      transform.getOrigin().z(),
      0, 0, 0, 0, 0);
    m_cf.trajectoryAdd(
      transform.getOrigin().x(),
      transform.getOrigin().y(),
      0,
      0, 0, 0, 0, 2 * 1000);
    m_cf.trajectoryStart();
    m_cf.setTrajectoryState(true);

    ros::Duration(2.0).sleep();

    m_cf.setTrajectoryState(false);

    return true;
  }

  void run()
  {
    // m_cf.reboot();

    m_cf.logReset();

    std::function<void(float)> cb_lq = std::bind(&CrazyflieROS::onLinkQuality, this, std::placeholders::_1);
    m_cf.setLinkQualityCallback(cb_lq);

    auto start = std::chrono::system_clock::now();

    if (m_enableParameters)
    {
      ROS_INFO("Requesting parameters...");
      m_cf.requestParamToc();
      for (auto iter = m_cf.paramsBegin(); iter != m_cf.paramsEnd(); ++iter) {
        auto entry = *iter;
        std::string paramName = "/" + m_tf_prefix + "/" + entry.group + "/" + entry.name;
        switch (entry.type) {
          case Crazyflie::ParamTypeUint8:
            ros::param::set(paramName, m_cf.getParam<uint8_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt8:
            ros::param::set(paramName, m_cf.getParam<int8_t>(entry.id));
            break;
          case Crazyflie::ParamTypeUint16:
            ros::param::set(paramName, m_cf.getParam<uint16_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt16:
            ros::param::set(paramName, m_cf.getParam<int16_t>(entry.id));
            break;
          case Crazyflie::ParamTypeUint32:
            ros::param::set(paramName, (int)m_cf.getParam<uint32_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt32:
            ros::param::set(paramName, m_cf.getParam<int32_t>(entry.id));
            break;
          case Crazyflie::ParamTypeFloat:
            ros::param::set(paramName, m_cf.getParam<float>(entry.id));
            break;
        }
      }
      ros::NodeHandle n;
      m_serviceUpdateParams = n.advertiseService(m_tf_prefix + "/update_params", &CrazyflieROS::updateParams, this);
    }

    std::unique_ptr<LogBlock<logImu> > logBlockImu;
    std::unique_ptr<LogBlock<log2> > logBlock2;
    std::vector<std::unique_ptr<LogBlockGeneric> > logBlocksGeneric(m_logBlocks.size());
    if (m_enableLogging) {

      std::function<void(const crtpPlatformRSSIAck*)> cb_ack = std::bind(&CrazyflieROS::onEmptyAck, this, std::placeholders::_1);
      m_cf.setEmptyAckCallback(cb_ack);

      ROS_INFO("Requesting Logging variables...");
      m_cf.requestLogToc();

      // std::function<void(logImu*)> cb = std::bind(&CrazyflieROS::onImuData, this, std::placeholders::_1);

      // logBlockImu.reset(new LogBlock<logImu>(
      //   &m_cf,{
      //     {"acc", "x"},
      //     {"acc", "y"},
      //     {"acc", "z"},
      //     {"gyro", "x"},
      //     {"gyro", "y"},
      //     {"gyro", "z"},
      //   }, cb));
      // logBlockImu->start(1); // 10ms

      // std::function<void(log2*)> cb2 = std::bind(&CrazyflieROS::onLog2Data, this, std::placeholders::_1);

      // logBlock2.reset(new LogBlock<log2>(
      //   &m_cf,{
      //     {"mag", "x"},
      //     {"mag", "y"},
      //     {"mag", "z"},
      //     {"baro", "temp"},
      //     {"baro", "pressure"},
      //     {"pm", "vbat"},
      //   }, cb2));
      // logBlock2->start(10); // 100ms

      // custom log blocks
      size_t i = 0;
      for (auto& logBlock : m_logBlocks)
      {
        std::function<void(std::vector<double>*, void* userData)> cb =
          std::bind(
            &CrazyflieROS::onLogCustom,
            this,
            std::placeholders::_1,
            std::placeholders::_2);

        logBlocksGeneric[i].reset(new LogBlockGeneric(
          &m_cf,
          logBlock.variables,
          (void*)&m_pubLogDataGeneric[i],
          cb));
        logBlocksGeneric[i]->start(logBlock.frequency / 10);
        ++i;
      }


    }

    ROS_INFO("Ready...");
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedSeconds = end-start;
    ROS_INFO("Elapsed: %f s", elapsedSeconds.count());

    m_listener.waitForTransform(m_worldFrame, m_frame, ros::Time(0), ros::Duration(10.0) );

    while(!m_isEmergency) {

      tf::StampedTransform transform;
      m_listener.lookupTransform(m_worldFrame, m_frame, ros::Time(0), transform);

      tfScalar current_euler_roll, current_euler_pitch, current_euler_yaw;
      tf::Matrix3x3(transform.getRotation()).getRPY(
          current_euler_roll,
          current_euler_pitch,
          current_euler_yaw);

      m_cf.sendPositionExternal(
        transform.getOrigin().x(),
        transform.getOrigin().y(),
        transform.getOrigin().z(),
        current_euler_yaw);

      // m_cf.sendPositionExternal(1.0, 2.0, 3.0, 4.0);

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void onImuData(logImu* data) {
    sensor_msgs::Imu msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = m_tf_prefix + "/base_link";
    msg.orientation_covariance[0] = -1;

    // measured in deg/s; need to convert to rad/s
    msg.angular_velocity.x = degToRad(data->gyro_x);
    msg.angular_velocity.y = degToRad(data->gyro_y);
    msg.angular_velocity.z = degToRad(data->gyro_z);

    // measured in mG; need to convert to m/s^2
    msg.linear_acceleration.x = data->acc_x * 9.81;
    msg.linear_acceleration.y = data->acc_y * 9.81;
    msg.linear_acceleration.z = data->acc_z * 9.81;

    m_pubImu.publish(msg);
  }

  void onLog2Data(log2* data) {

    {
      sensor_msgs::Temperature msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = m_tf_prefix + "/base_link";
      // measured in degC
      msg.temperature = data->baro_temp;
      m_pubTemp.publish(msg);
    }

    {
      sensor_msgs::MagneticField msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = m_tf_prefix + "/base_link";

      // measured in Tesla
      msg.magnetic_field.x = data->mag_x;
      msg.magnetic_field.y = data->mag_y;
      msg.magnetic_field.z = data->mag_z;
      m_pubMag.publish(msg);
    }

    {
      std_msgs::Float32 msg;
      // hPa (=mbar)
      msg.data = data->baro_pressure;
      m_pubPressure.publish(msg);
    }

    {
      std_msgs::Float32 msg;
      // V
      msg.data = data->pm_vbat;
      m_pubBattery.publish(msg);
    }
  }

  void onLogCustom(std::vector<double>* values, void* userData) {

    ros::Publisher* pub = reinterpret_cast<ros::Publisher*>(userData);

    crazyflie_driver::GenericLogData msg;
    msg.values = *values;

    pub->publish(msg);
  }

  void onEmptyAck(const crtpPlatformRSSIAck* data) {
      std_msgs::Float32 msg;
      // dB
      msg.data = data->rssi;
      m_pubRssi.publish(msg);
  }

  void onLinkQuality(float linkQuality) {
      if (linkQuality < 0.7) {
        ROS_WARN("Link Quality low (%f)", linkQuality);
      }
  }

private:
  Crazyflie m_cf;
  std::string m_tf_prefix;
  std::string m_frame;
  std::string m_worldFrame;
  bool m_isEmergency;
  float m_roll_trim;
  float m_pitch_trim;
  bool m_enableLogging;
  bool m_enableParameters;
  std::vector<crazyflie_driver::LogBlock> m_logBlocks;

  ros::ServiceServer m_serviceEmergency;
  ros::ServiceServer m_serviceUpdateParams;
  ros::ServiceServer m_serviceUploadTrajectory;
  ros::ServiceServer m_serviceStartTrajectory;
  ros::ServiceServer m_serviceTakeoff;
  ros::ServiceServer m_serviceLand;
  ros::Subscriber m_subscribeCmdVel;
  ros::Publisher m_pubImu;
  ros::Publisher m_pubTemp;
  ros::Publisher m_pubMag;
  ros::Publisher m_pubPressure;
  ros::Publisher m_pubBattery;
  ros::Publisher m_pubRssi;
  std::vector<ros::Publisher> m_pubLogDataGeneric;

  tf::TransformListener m_listener;
};

bool add_crazyflie(
  crazyflie_driver::AddCrazyflie::Request  &req,
  crazyflie_driver::AddCrazyflie::Response &res)
{
  ROS_INFO("Adding %s as %s with trim(%f, %f). Logging: %d, Parameters: %d, frame: %s, world_frame: %s",
    req.uri.c_str(),
    req.tf_prefix.c_str(),
    req.roll_trim,
    req.pitch_trim,
    req.enable_parameters,
    req.enable_logging,
    req.frame.c_str(),
    req.world_frame.c_str());

  // Leak intentionally
  CrazyflieROS* cf = new CrazyflieROS(
    req.uri,
    req.tf_prefix,
    req.frame,
    req.world_frame,
    req.roll_trim,
    req.pitch_trim,
    req.enable_logging,
    req.enable_parameters,
    req.log_blocks);

  return true;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "crazyflie_server");
  ros::NodeHandle n;

  ros::ServiceServer service = n.advertiseService("add_crazyflie", add_crazyflie);
  ros::spin();

  return 0;
}
