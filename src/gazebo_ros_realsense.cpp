#include "realsense_gazebo_plugin/gazebo_ros_realsense.h"
#include <sensor_msgs/fill_image.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>


namespace
{
std::string extractCameraName(const std::string & name);
sensor_msgs::msg::CameraInfo cameraInfo(
  const sensor_msgs::msg::Image & image,
  float horizontal_fov);
}

namespace gazebo
{
// Register the plugin
GZ_REGISTER_MODEL_PLUGIN(GazeboRosRealsense)

GazeboRosRealsense::GazeboRosRealsense()
{
  std::cout << "CONSTRUCTED GAZEBO PLUGIN" << std::endl;
}

GazeboRosRealsense::~GazeboRosRealsense()
{
  RCLCPP_DEBUG_STREAM(this->node_->get_logger(), "realsense_camera Unloaded");
}

void GazeboRosRealsense::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{

  this->model_ = _model;

  this->node_ = rclcpp::Node::make_shared("GazeboRealsenseNode");

  // Make sure the ROS node for Gazebo has already been initialized
  if (!rclcpp::ok()) {
    std::cout << "ERROR : ROS IS NOT OK";
    RCLCPP_ERROR(
      node_->get_logger(),
      "A ROS node for Gazebo has not been initialized, unable "
      "to load plugin. "
      "Load the Gazebo system plugin "
      "'libgazebo_ros_api_plugin.so' in the gazebo_ros "
      "package");
    return;
  }
  RCLCPP_ERROR(node_->get_logger(), "Realsense Gazebo ROS plugin loading.");
  std::cout << "ROS IS OK LOADING REALSENSE PLUGIN" << std::endl;


  RealSensePlugin::Load(_model, _sdf);

  std::cout << "LOADED REALSENSE PLUGIN" << std::endl;


  // initialize camera_info_manager
  this->camera_info_manager_.reset(
    new camera_info_manager::CameraInfoManager(this->node_.get(), this->GetHandle()));

  this->itnode_ = new image_transport::ImageTransport(this->node_);

  this->color_pub_ = this->itnode_->advertiseCamera(
    cameraParamsMap_[COLOR_CAMERA_NAME].topic_name, 2);
  this->ir1_pub_ = this->itnode_->advertiseCamera(
    cameraParamsMap_[IRED1_CAMERA_NAME].topic_name, 2);
  this->ir2_pub_ = this->itnode_->advertiseCamera(
    cameraParamsMap_[IRED2_CAMERA_NAME].topic_name, 2);
  this->depth_pub_ = this->itnode_->advertiseCamera(
    cameraParamsMap_[DEPTH_CAMERA_NAME].topic_name, 2);

  if (pointCloud_) {
    this->pointcloud_pub_ = this->node_->create_publisher<sensor_msgs::msg::PointCloud2>(
      pointCloudTopic_, rclcpp::SystemDefaultsQoS());
  }
  if (pose_) {
    this->pose_pub_ = this->node_->create_publisher<geometry_msgs::msg::PoseStamped>(
      poseTopic_, rclcpp::SystemDefaultsQoS());
  }

  std::cout << "SETTED UP ALL PUBLISHERS" << std::endl;

}

void GazeboRosRealsense::OnNewFrame(
  const rendering::CameraPtr cam,
  const transport::PublisherPtr pub)
{
  //auto start = std::chrono::high_resolution_clock::now();
  common::Time current_time = this->world->SimTime();

  // identify camera
  std::string camera_id = extractCameraName(cam->Name());
  const std::map<std::string, image_transport::CameraPublisher *>
  camera_publishers = {
    {COLOR_CAMERA_NAME, &(this->color_pub_)},
    {IRED1_CAMERA_NAME, &(this->ir1_pub_)},
    {IRED2_CAMERA_NAME, &(this->ir2_pub_)},
  };
  const auto image_pub = camera_publishers.at(camera_id);

  // copy data into image
  this->image_msg_.header.frame_id =
    this->cameraParamsMap_[camera_id].optical_frame;
  this->image_msg_.header.stamp.sec = current_time.sec;
  this->image_msg_.header.stamp.nanosec = current_time.nsec;

  // set image encoding
  const std::map<std::string, std::string> supported_image_encodings = {
    {"RGB_INT8", sensor_msgs::image_encodings::RGB8},
    {"L_INT8", sensor_msgs::image_encodings::TYPE_8UC1}};
  const auto pixel_format = supported_image_encodings.at(cam->ImageFormat());

  // copy from simulation image to ROS msg
  sensor_msgs::fillImage(
    this->image_msg_, pixel_format, cam->ImageHeight(),
    cam->ImageWidth(), cam->ImageDepth() * cam->ImageWidth(),
    reinterpret_cast<const void *>(cam->ImageData()));

  // identify camera rendering
  const std::map<std::string, rendering::CameraPtr> cameras = {
    {COLOR_CAMERA_NAME, this->colorCam},
    {IRED1_CAMERA_NAME, this->ired1Cam},
    {IRED2_CAMERA_NAME, this->ired2Cam},
  };
  //auto stop = std::chrono::high_resolution_clock::now();
  //auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
  //std::cout << "RGB duration " << duration.count() << " \u03BCs" << std::endl;
  // publish to ROS
  auto camera_info_msg =
    cameraInfo(this->image_msg_, cameras.at(camera_id)->HFOV().Radian());
  
  if (pose_){
    auto model_state = std::make_unique<gazebo::physics::ModelState>(this->model_);
    auto pose = std::make_unique<ignition::math::Pose3<double>>(model_state->Pose());
  
    this->pose_msg_.header.frame_id = std::string{"odom"};
    this->pose_msg_.header.stamp.sec = current_time.sec;
    this->pose_msg_.header.stamp.nanosec = current_time.nsec;
    this->pose_msg_.pose.position.x = pose->Pos().X();
    this->pose_msg_.pose.position.y = pose->Pos().Y();
    this->pose_msg_.pose.position.z = pose->Pos().Z();
    this->pose_msg_.pose.orientation.x = pose->Rot().X();
    this->pose_msg_.pose.orientation.y = pose->Rot().Y();
    this->pose_msg_.pose.orientation.z = pose->Rot().Z();
    this->pose_msg_.pose.orientation.w = pose->Rot().W();
  
    pose_pub_->publish(this->pose_msg_);
  }
  
  image_pub->publish(this->image_msg_, camera_info_msg);
}

// Referenced from gazebo_plugins
// https://github.com/ros-simulation/gazebo_ros_pkgs/blob/kinetic-devel/gazebo_plugins/src/gazebo_ros_openni_kinect.cpp#L302
// Fill depth information
bool GazeboRosRealsense::FillPointCloudHelper(
  sensor_msgs::msg::PointCloud2 & point_cloud_msg,
  uint32_t rows_arg, uint32_t cols_arg,
  uint32_t step_arg, const void * data_arg)
{
  double pointCloudCutOff_ = (double)this->rangeMinDepth_;
  double pointCloudCutOffMax_ = (double)this->rangeMaxDepth_;
  sensor_msgs::PointCloud2Modifier pcd_modifier(point_cloud_msg);
  if (colorCloud_){
      pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");
  } else {
      pcd_modifier.setPointCloud2FieldsByString(1, "xyz");
  }
  // convert to flat array shape, we need to reconvert later
  pcd_modifier.resize(rows_arg * cols_arg);
  point_cloud_msg.is_dense = true;

  sensor_msgs::PointCloud2Iterator<float> iter_x(pointcloud_msg_, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(pointcloud_msg_, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(pointcloud_msg_, "z");
  std::unique_ptr<sensor_msgs::PointCloud2Iterator<uint8_t>> iter_rgb_ptr;
  
  if (colorCloud_){
      iter_rgb_ptr = std::make_unique<sensor_msgs::PointCloud2Iterator<uint8_t>>(pointcloud_msg_, "rgb");
  }

  float * toCopyFrom = (float *)data_arg;
  int index = 0;

  double hfov = this->depthCam->HFOV().Radian();
  double fl = ((double)this->depthCam->ImageWidth()) / (2.0 * tan(hfov / 2.0));
  

  // convert depth to point cloud
  for (uint32_t j = 0; j < rows_arg; j++) {
    double pAngle;
    if (rows_arg > 1) {
      pAngle = atan2((double)j - 0.5 * (double)(rows_arg - 1), fl);
    } else {
      pAngle = 0.0;
    }
    for (uint32_t i = 0; i < cols_arg; i++, ++iter_x, ++iter_y, ++iter_z) {
      double yAngle;
      if (cols_arg > 1) {
        yAngle = atan2((double)i - 0.5 * (double)(cols_arg - 1), fl);
      } else {
        yAngle = 0.0;
      }

      double depth = toCopyFrom[index++];  // + 0.0*this->myParent->GetNearClip();



      if (depth > pointCloudCutOff_ && depth < pointCloudCutOffMax_) {
        // in optical frame
        // hardcoded rotation rpy(-M_PI/2, 0, -M_PI/2) is built-in
        // to urdf, where the *_optical_frame should have above relative
        // rotation from the physical camera *_frame
        *iter_x = depth * tan(yAngle);
        *iter_y = depth * tan(pAngle);
        *iter_z = depth;
      } else { // point in the unseeable range
        *iter_x = *iter_y = *iter_z = std::numeric_limits<float>::quiet_NaN();
        point_cloud_msg.is_dense = false;
      }

      if (colorCloud_){
          // put image color data for each point
          sensor_msgs::PointCloud2Iterator<uint8_t>& iter_rgb = *iter_rgb_ptr;
          uint8_t * image_src = (uint8_t *)(&(this->image_msg_.data[0]));
          if (this->image_msg_.data.size() == rows_arg * cols_arg * 3) {
            // color
            iter_rgb[0] = image_src[i * 3 + j * cols_arg * 3 + 0];
            iter_rgb[1] = image_src[i * 3 + j * cols_arg * 3 + 1];
            iter_rgb[2] = image_src[i * 3 + j * cols_arg * 3 + 2];
          } else if (this->image_msg_.data.size() == rows_arg * cols_arg) {
            // mono (or bayer?  @todo; fix for bayer)
            iter_rgb[0] = image_src[i + j * cols_arg];
            iter_rgb[1] = image_src[i + j * cols_arg];
            iter_rgb[2] = image_src[i + j * cols_arg];
          } else {
            // no image
            iter_rgb[0] = 0;
            iter_rgb[1] = 0;
            iter_rgb[2] = 0;
          }
          ++iter_rgb;
      }
    }
  }

  // reconvert to original height and width after the flat reshape
  point_cloud_msg.height = rows_arg;
  point_cloud_msg.width = cols_arg;
  point_cloud_msg.row_step = point_cloud_msg.point_step * point_cloud_msg.width;

  return true;
}

void GazeboRosRealsense::OnNewDepthFrame()
{
  // get current time
  //auto start = std::chrono::high_resolution_clock::now();
  common::Time current_time = this->world->SimTime();

  RealSensePlugin::OnNewDepthFrame();

  // copy data into image
  this->depth_msg_.header.frame_id =
    this->cameraParamsMap_[DEPTH_CAMERA_NAME].optical_frame;
  this->depth_msg_.header.stamp.sec = current_time.sec;
  this->depth_msg_.header.stamp.nanosec = current_time.nsec;

  // set image encoding
  std::string pixel_format = sensor_msgs::image_encodings::TYPE_16UC1;

  // copy from simulation image to ROS msg
  sensor_msgs::fillImage(
    this->depth_msg_, pixel_format, this->depthCam->ImageHeight(),
    this->depthCam->ImageWidth(), 2 * this->depthCam->ImageWidth(),
    reinterpret_cast<const void *>(this->depthMap.data()));

  // publish to ROS
  auto depth_info_msg =
    cameraInfo(this->depth_msg_, this->depthCam->HFOV().Radian());
  this->depth_pub_.publish(this->depth_msg_, depth_info_msg);

  if ((pointCloud_ && this->pointcloud_pub_->get_subscription_count() > 0) || forceCloud_) {
    this->pointcloud_msg_.header = this->depth_msg_.header;
    this->pointcloud_msg_.width = this->depthCam->ImageWidth();
    this->pointcloud_msg_.height = this->depthCam->ImageHeight();
    this->pointcloud_msg_.row_step =
      this->pointcloud_msg_.point_step * this->depthCam->ImageWidth();
    FillPointCloudHelper(
      this->pointcloud_msg_, this->depthCam->ImageHeight(),
      this->depthCam->ImageWidth(), 2 * this->depthCam->ImageWidth(),
      (const void *)this->depthCam->DepthData());
    //auto stop = std::chrono::high_resolution_clock::now();
    //auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    //std::cout << "Depth and pc duration " << duration.count() << "ms" << std::endl;

    this->pointcloud_pub_->publish(this->pointcloud_msg_);
  }

  
}
}

namespace
{
std::string extractCameraName(const std::string & name)
{
  if (name.find(COLOR_CAMERA_NAME) != std::string::npos) {
    return COLOR_CAMERA_NAME;
  }
  if (name.find(IRED1_CAMERA_NAME) != std::string::npos) {
    return IRED1_CAMERA_NAME;
  }
  if (name.find(IRED2_CAMERA_NAME) != std::string::npos) {
    return IRED2_CAMERA_NAME;
  }

  RCLCPP_ERROR(rclcpp::get_logger("realsense_camera"), "Unknown camera name");

  return COLOR_CAMERA_NAME;
}

sensor_msgs::msg::CameraInfo cameraInfo(
  const sensor_msgs::msg::Image & image,
  float horizontal_fov)
{
  sensor_msgs::msg::CameraInfo info_msg;

  info_msg.header = image.header;
  info_msg.distortion_model = "plumb_bob";
  info_msg.height = image.height;
  info_msg.width = image.width;

  float focal = 0.5 * image.width / tan(0.5 * horizontal_fov);

  info_msg.k[0] = focal;
  info_msg.k[4] = focal;
  info_msg.k[2] = info_msg.width * 0.5;
  info_msg.k[5] = info_msg.height * 0.5;
  info_msg.k[8] = 1.;

  info_msg.p[0] = info_msg.k[0];
  info_msg.p[5] = info_msg.k[4];
  info_msg.p[2] = info_msg.k[2];
  info_msg.p[6] = info_msg.k[5];
  info_msg.p[10] = info_msg.k[8];

  //    info_msg.roi.do_rectify = true;

  return info_msg;
}
}
