#include <ros/ros.h>
#include <sensor_msgs/TimeReference.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/distortion_models.h>
#include <sensor_msgs/PointCloud2.h>
#include <image_transport/image_transport.h>
#include <camera_info_manager/camera_info_manager.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>
#include <dynamic_reconfigure/server.h>
#include <People_counter/camConfig.h>
#include <iostream>
#include <fstream>
#include <time.h>
#include <boost/thread.hpp>
#include <image_geometry/stereo_camera_model.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/ximgproc/disparity_filter.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/features2d.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include "withrobot_camera.hpp"
#include "myahrs_plus.hpp"

using namespace cv;
using namespace cv::xfeatures2d;
using namespace cv::ximgproc;
using namespace std;

class StereoCamera
{
    Withrobot::Camera* camera;
    Withrobot::camera_format camFormat;
 
private:

    std::string devPath_;

public:
    int width_;
    int height_;
 
    StereoCamera(int resolution, double frame_rate): camera(NULL)
    {
        enum_dev_list();

        camera = new Withrobot::Camera(devPath_.c_str());

        if (resolution == 0) { width_ = 1280; height_ = 960;}
        if (resolution == 1) { width_ = 1280; height_ = 720;}
        if (resolution == 2) { width_ = 640; height_  = 480;}
        if (resolution == 3) { width_ = 640; height_  = 360;}
        if (resolution == 4) { width_ = 320; height_  = 240;}

        camera->set_format(width_, height_, Withrobot::fourcc_to_pixformat('Y','U','Y','V'), 1, (unsigned int)frame_rate);

        /*
         * get current camera format (image size and frame rate)
         */
        camera->get_current_format(camFormat);

        camFormat.print();

        /* Withrobot camera start */
        camera->start();
    }

    ~StereoCamera()
    {
        camera->stop();
        delete camera;
    }

    void enum_dev_list()
    {
        /* enumerate device(UVC compatible devices) list */
        std::vector<Withrobot::usb_device_info> dev_list;
        int dev_num = Withrobot::get_usb_device_info_list(dev_list);

        if (dev_num < 1)
        {
            dev_list.clear();

            return;
        }

        for (unsigned int i=0; i < dev_list.size(); i++)
        {
            if (dev_list[i].product == "oCamS-1CGN-U")
            {
                devPath_ = dev_list[i].dev_node;
                return;
            }
        }
    }

    void uvc_control(int exposure, int gain, int blue, int red, bool ae)
    {
        /* Exposure Setting */
        camera->set_control("Exposure (Absolute)", exposure);

        /* Gain Setting */
        camera->set_control("Gain", gain);

        /* White Balance Setting */
        camera->set_control("White Balance Blue Component", blue);
        camera->set_control("White Balance Red Component", red);

        /* Auto Exposure Setting */
        if (ae)
            camera->set_control("Exposure, Auto", 0x3);
        else
            camera->set_control("Exposure, Auto", 0x1);
    }

    bool getImages(cv::Mat &left_image, cv::Mat &right_image, uint32_t &time_stamp) {

        cv::Mat srcImg(cv::Size(camFormat.width, camFormat.height), CV_8UC2);
        cv::Mat dstImg[2];

        uint32_t ts;

        if (camera->get_frame(srcImg.data, camFormat.image_size, 1) != -1)
        {
            // time stamp
            memcpy(&ts, srcImg.data, sizeof(ts));

            cv::split(srcImg, dstImg);

            time_stamp = ts;
            right_image = dstImg[0];
            left_image = dstImg[1];

            return true;
        } else {
            return false;
        }
    }
};


using namespace WithrobotIMU;
class MyAhrsDriverForROS : public iMyAhrsPlus
{
public:
    SensorData sensor_data_;
private:
    ros::NodeHandle nh_;
    ros::NodeHandle nh_priv_;

    ros::Publisher time_stamp_pub_;
    ros::Publisher imu_data_pub_;
    ros::Publisher imu_mag_pub_;

    tf::TransformBroadcaster broadcaster_;

    Platform::Mutex lock_;
    //SensorData sensor_data_;

    std::string parent_frame_id_;
    std::string frame_id_;
    double linear_acceleration_stddev_;
    double angular_velocity_stddev_;
    double magnetic_field_stddev_;
    double orientation_stddev_;

    void OnSensorData(int sensor_id, SensorData data)
    {
        LockGuard _l(lock_);
        sensor_data_ = data;
        publish_topic();
    }

    void OnAttributeChange(int sensor_id, std::string attribute_name, std::string value)
    {
        printf("OnAttributeChange(id %d, %s, %s)\n", sensor_id, attribute_name.c_str(), value.c_str());
    }

public:
    MyAhrsDriverForROS(std::string port="", int baud_rate=115200)
        : iMyAhrsPlus(port, baud_rate),
          nh_priv_("~")
    {
        // dependent on user device
        nh_priv_.setParam("port", port);
        nh_priv_.setParam("baud_rate", baud_rate);

        // default frame id
        nh_priv_.param("frame_id", frame_id_, std::string("imu_link"));

        // for testing the tf
        nh_priv_.param("parent_frame_id_", parent_frame_id_, std::string("base_link"));

        // defaults obtained experimentally from device
        nh_priv_.param("linear_acceleration_stddev", linear_acceleration_stddev_, -1.0);
        nh_priv_.param("angular_velocity_stddev", angular_velocity_stddev_, -1.0);
        nh_priv_.param("magnetic_field_stddev", magnetic_field_stddev_, -1.0);
        nh_priv_.param("orientation_stddev", orientation_stddev_, -1.0);

        // publisher for streaming
        time_stamp_pub_     = nh_.advertise<sensor_msgs::TimeReference>("imu/timestamp",1);
        imu_data_pub_       = nh_.advertise<sensor_msgs::Imu>("imu/data", 1);
        imu_mag_pub_        = nh_.advertise<sensor_msgs::MagneticField>("imu/mag", 1);

    }

    ~MyAhrsDriverForROS()
    {}

    bool initialize(std::string mode="")
    {
        bool ok = false;

        do
        {
            if(start() == false) break;

            /* IMU mode */
            if(cmd_data_format(mode.c_str()) == false) break;
            printf("IMU initialized: %s\r\n", mode.c_str());
            ok = true;
        } while(0);

        return ok;
    }

    inline void get_data(SensorData& data)
    {
        LockGuard _l(lock_);
        data = sensor_data_;
    }

    inline SensorData get_data()
    {
        LockGuard _l(lock_);
        return sensor_data_;
    }

    void publish_topic()
    {
        uint32_t time_stamp, sec, nsec;

        sensor_msgs::TimeReference time_stamp_msg;
        sensor_msgs::Imu imu_data_msg;
        sensor_msgs::MagneticField imu_magnetic_msg;

        double linear_acceleration_cov = 0.05;
        double angular_velocity_cov    = 0.025;
        double magnetic_field_cov      = -1;
        double orientation_cov         = 0.1;

        imu_data_msg.linear_acceleration_covariance[0] =
                imu_data_msg.linear_acceleration_covariance[4] =
                imu_data_msg.linear_acceleration_covariance[8] = linear_acceleration_cov;

        imu_data_msg.angular_velocity_covariance[0] =
                imu_data_msg.angular_velocity_covariance[4] =
                imu_data_msg.angular_velocity_covariance[8] = angular_velocity_cov;

        imu_data_msg.orientation_covariance[0] =
                imu_data_msg.orientation_covariance[4] =
                imu_data_msg.orientation_covariance[8] = orientation_cov;

        imu_magnetic_msg.magnetic_field_covariance[0] =
                imu_magnetic_msg.magnetic_field_covariance[4] =
                imu_magnetic_msg.magnetic_field_covariance[8] = magnetic_field_cov;

        ros::Time now = ros::Time::now();

        /* time stamp publish */
        time_stamp = sensor_data_.time_stamp;
        sec = (uint32_t)time_stamp/1000;
        nsec = (uint32_t)(time_stamp - sec*1000) * 1e6;

        ros::Time measurement_time(sec, nsec);
        ros::Time time_ref(0, 0);
        time_stamp_msg.header.stamp = measurement_time;
        time_stamp_msg.header.frame_id = frame_id_;
        time_stamp_msg.time_ref = time_ref;
        time_stamp_pub_.publish(time_stamp_msg);


        now = measurement_time;

        imu_data_msg.header.stamp     =
                imu_magnetic_msg.header.stamp = now;

        imu_data_msg.header.frame_id = frame_id_;


        // orientation
        imu_data_msg.orientation.x = float(sensor_data_.quaternion.x) / 16384.;
        imu_data_msg.orientation.y = float(sensor_data_.quaternion.y) / 16384.;
        imu_data_msg.orientation.z = float(sensor_data_.quaternion.z) / 16384.;
        imu_data_msg.orientation.w = float(sensor_data_.quaternion.w) / 16384.;

        // original data used the g unit, convert to m/s^2
        imu_data_msg.linear_acceleration.x     = float(sensor_data_.imu.ax) / 100.;
        imu_data_msg.linear_acceleration.y     = float(sensor_data_.imu.ay) / 100.;
        imu_data_msg.linear_acceleration.z     = float(sensor_data_.imu.az) / 100.;

        // original data used the degree/s unit, convert to radian/s
        imu_data_msg.angular_velocity.x     = float(sensor_data_.imu.gx) / 900.;
        imu_data_msg.angular_velocity.y     = float(sensor_data_.imu.gy) / 900.;
        imu_data_msg.angular_velocity.z     = float(sensor_data_.imu.gz) / 900.;

        // original data used the uTesla unit, convert to Tesla
        imu_magnetic_msg.magnetic_field.x = float(sensor_data_.imu.mx) / 16.;
        imu_magnetic_msg.magnetic_field.y = float(sensor_data_.imu.my) / 16.;
        imu_magnetic_msg.magnetic_field.z = float(sensor_data_.imu.mz) / 16.;

        // publish the IMU data
        imu_data_pub_.publish(imu_data_msg);
        imu_mag_pub_.publish(imu_magnetic_msg);

        // publish tf
        broadcaster_.sendTransform(tf::StampedTransform(tf::Transform(tf::Quaternion(imu_data_msg.orientation.x, imu_data_msg.orientation.y, imu_data_msg.orientation.z, imu_data_msg.orientation.w),
                                                                      tf::Vector3(0.0, 0.0, 0.0)), now, parent_frame_id_, frame_id_));
    }
};


class oCamStereoROS {
    enum MatchingAlg{ STEREO_BM=0, STEREO_SGBM=1, STEREO_HH=2, STEREO_3WAY=3, STEREO_HH4=4 };
private:
    struct StereoParams
    {
        MatchingAlg mode;
        int preFilterType;
        int preFilterSize;
        int preFilterCap;
        int SADWindowSize;
        int minDisparity;
        int numDisparities;
        int textureThreshold;
        int uniquenessRatio;
        int speckleRange;
        int speckleWindowSize;
        int disp12MaxDiff;
        int p1;
        int p2;

    };
    StereoParams state;
    Withrobot::Camera* camera_ros;
    int resolution_;
    double frame_rate_;
    int exposure_, gain_, wb_blue_, wb_red_, roi_x, roi_y, roi_w, roi_h;
    int detect_upline_x1 = 0;
    int detect_upline_y1 = 10;
    int detect_upline_x2 = 149;
    int detect_upline_y2 = 30;

    int detect_downline_x1 = 0;
    int detect_downline_y1 = 170;
    int detect_downline_x2 = 149;
    int detect_downline_y2 = 180;
    bool autoexposure_;
    bool show_image_;
    bool config_changed_;

    bool status_ent = false;
    bool status_out = false;
    bool check_status_ent = false;
    bool check_status_out = false;
    bool check_self_ent = false;
    bool check_self_out = false;

    bool check_height = false;
    bool pass_status_ent = false;
    bool pass_status_out = false;

    int cnt_ent = false;
    int cnt_out = false;
    int status_cnt = 0;
    int count_people_ent = 0;
    int count_people_out = 0;

    float min_height = 0.0f;
    float ceiling_height = 0.0f;
    cv::Mat dispbgr = cv::Mat::zeros(320, 240, CV_8UC1);
    ros::NodeHandle nh;
    std::string left_frame_id_, right_frame_id_;
    StereoCamera* ocams;
    MyAhrsDriverForROS* IMU;

    /* \brief Image to ros message conversion
     * \param img : the image to publish
     * \param encodingType : the sensor_msgs::image_encodings encoding type
     * \param frameId : the id of the reference frame of the image
     * \param t : the ros::Time to stamp the image
     */
    sensor_msgs::ImagePtr imageToROSmsg(cv::Mat img, const std::string encodingType, std::string frameId, ros::Time t)
    {
        sensor_msgs::ImagePtr ptr = boost::make_shared<sensor_msgs::Image>();
        sensor_msgs::Image& imgMessage = *ptr;
        imgMessage.header.stamp = t;
        imgMessage.header.frame_id = frameId;
        imgMessage.height = img.rows;
        imgMessage.width = img.cols;
        imgMessage.encoding = encodingType;
        int num = 1; //for endianness detection
        imgMessage.is_bigendian = !(*(char *) &num == 1);
        imgMessage.step = img.cols * img.elemSize();
        size_t size = imgMessage.step * img.rows;
        imgMessage.data.resize(size);

        if (img.isContinuous())
            memcpy((char*) (&imgMessage.data[0]), img.data, size);
        else {
            uchar* opencvData = img.data;
            uchar* rosData = (uchar*) (&imgMessage.data[0]);
            for (unsigned int i = 0; i < img.rows; i++) {
                memcpy(rosData, opencvData, imgMessage.step);
                rosData += imgMessage.step;
                opencvData += img.step;
            }
        }
        return ptr;
    }

#if 1
    void publishCamInfo(const ros::Publisher& pub_cam_info, sensor_msgs::CameraInfo& cam_info_msg, ros::Time t)
    {
        cam_info_msg.header.stamp = t;
        pub_cam_info.publish(cam_info_msg);
    }
#else
    /* \brief Publish the informations of a camera with a ros Publisher
     * \param cam_info_msg : the information message to publish
     * \param pub_cam_info : the publisher object to use
     * \param t : the ros::Time to stamp the message
     */
    void publishCamInfo(sensor_msgs::CameraInfoPtr cam_info_msg, ros::Publisher pub_cam_info, ros::Time t)
    {
        static int seq = 0;
        cam_info_msg->header.stamp = t;
        cam_info_msg->header.seq = seq;
        pub_cam_info.publish(cam_info_msg);
        seq++;
    }
#endif

    /* \brief Publish a cv::Mat image with a ros Publisher
     * \param img : the image to publish
     * \param pub_img : the publisher object to use
     * \param img_frame_id : the id of the reference frame of the image
     * \param t : the ros::Time to stamp the image
     */
    void publishImage(cv::Mat img, image_transport::Publisher &pub_img, std::string img_frame_id, ros::Time t, std::string encoding_id)
    {
        pub_img.publish(imageToROSmsg(img, encoding_id, img_frame_id, t));
    }

    /* \brief Publish a cv::Mat depth image with a ros Publisher
     * \param depth : the depth image to publish
     * \param pub_depth : the publisher object to use
     * \param depth_frame_id : the id of the reference frame of the depth image
     * \param t : the ros::Time to stamp the depth image
     */
    void publishDepth(cv::Mat depth, image_transport::Publisher &pub_depth, std::string depth_frame_id, ros::Time t, bool openniDepthMode)
    {
        std::string encoding;
#if 1
        if (openniDepthMode) {
            depth *= 1000.0f;
            depth.convertTo(depth, CV_16UC1); // in mm, rounded
            encoding = sensor_msgs::image_encodings::TYPE_16UC1;
        } else {
            encoding = sensor_msgs::image_encodings::TYPE_32FC1;
        }
#else
        encoding = sensor_msgs::image_encodings::TYPE_32FC1;
#endif
        pub_depth.publish(imageToROSmsg(depth, encoding, depth_frame_id, t));
    }

    inline bool isValidPoint(const cv::Vec3f& pt)
    {
        // Check both for disparities explicitly marked as invalid (where OpenCV maps pt.z to MISSING_Z)
        // and zero disparities (point mapped to infinity).
        return pt[2] != image_geometry::StereoCameraModel::MISSING_Z && !std::isinf(pt[2]);
    }


    void Rectification(cv::Mat& src_l, cv::Mat& src_r, cv::Mat& _map11, cv::Mat& _map12, cv::Mat& _map21, cv::Mat& _map22, cv::Mat& dst_l, cv::Mat& dst_r)
    {
        cv::Mat left = src_l;
        cv::Mat right = src_r;

        cv::Mat map11=_map11;
        cv::Mat map12=_map12;
        cv::Mat map21=_map21;
        cv::Mat map22=_map22;

        cv::Mat img1r, img2r;
        cv::remap(left, img1r, map11, map12, cv::INTER_LINEAR);
        cv::remap(right, img2r, map21, map22, cv::INTER_LINEAR);

        dst_l = img1r;
        dst_r = img2r;
    }

    void stereoMatch(cv::Mat left_mono, cv::Mat right_mono, cv::Mat& disparity, MatchingAlg alg = STEREO_BM)
    {
        cv::Mat left = left_mono;
        cv::Mat right = right_mono;
        cv::Mat disp;

        cv::Mat disp_2;
        cv::Mat disp_filtered;
        cv::Mat conf_map = Mat(left.rows,left.cols,CV_8U);
        conf_map = Scalar(255);
        double lambda = 300.0;
        double sigma = 1.5;

        cv::Ptr<cv::StereoBM> bm = cv::StereoBM::create();
        //cv::Ptr<DisparityWLSFilter> wls_filter;
        //wls_filter = createDisparityWLSFilter(bm);
        //Ptr<StereoMatcher> right_matcher = createRightMatcher(bm);

        if( alg == STEREO_BM )
        {
            // pre-filter
            bm->setPreFilterType(state.preFilterType);
            bm->setPreFilterSize(state.preFilterSize);
            bm->setPreFilterCap(state.preFilterCap);
            bm->setTextureThreshold(state.textureThreshold);
            bm->setUniquenessRatio(state.uniquenessRatio);

            bm->setMinDisparity(state.minDisparity);
            bm->setNumDisparities(state.numDisparities);
            bm->setBlockSize(state.SADWindowSize);
            bm->setSpeckleWindowSize(state.speckleWindowSize);
            bm->setSpeckleRange(state.speckleRange);
            bm->setDisp12MaxDiff(state.disp12MaxDiff);
            //            bm->setROI1(roi1);
            //            bm->setROI2(roi2);

            bm->compute(left, right, disp);
            //bm->compute(right, left, disp_2);
            //right_matcher->compute(right , left, disp_2);
        }
        else if( alg == STEREO_SGBM || alg == STEREO_HH || alg == STEREO_3WAY || alg == STEREO_HH4 )
        {
            cv::Ptr<cv::StereoSGBM> sgbm = cv::StereoSGBM::create();

            sgbm->setMinDisparity(state.minDisparity);
            sgbm->setNumDisparities(state.numDisparities);
            sgbm->setBlockSize(state.SADWindowSize);
            sgbm->setP1(state.p1);
            sgbm->setP2(state.p2);
            sgbm->setDisp12MaxDiff(state.disp12MaxDiff);
            sgbm->setPreFilterCap(state.preFilterCap);
            sgbm->setUniquenessRatio(state.uniquenessRatio);
            sgbm->setSpeckleWindowSize(state.speckleWindowSize);
            sgbm->setSpeckleRange(state.speckleRange);

            if(alg==STEREO_HH)
                sgbm->setMode(cv::StereoSGBM::MODE_HH);
            else if(alg==STEREO_SGBM)
                sgbm->setMode(cv::StereoSGBM::MODE_SGBM);
            else if(alg==STEREO_3WAY)
                sgbm->setMode(cv::StereoSGBM::MODE_SGBM_3WAY);
            else if(alg==STEREO_HH4)
                sgbm->setMode(cv::StereoSGBM::MODE_HH4);

            sgbm->compute(left, right, disp);
        }

        //! [filtering]
        //conf_map = wls_filter->getConfidenceMap();
        disparity = disp;
    }

    void device_poll() {
        //Reconfigure confidence
        dynamic_reconfigure::Server<People_counter::camConfig> server;
        dynamic_reconfigure::Server<People_counter::camConfig>::CallbackType f;
        f = boost::bind(&oCamStereoROS::callback, this ,_1, _2);
        server.setCallback(f);

        // setup publisher stuff
        image_transport::ImageTransport it(nh);
        image_transport::Publisher left_image_pub = it.advertise("stereo/left/image_raw", 1);
        image_transport::Publisher right_image_pub = it.advertise("stereo/right/image_raw", 1);
        image_transport::Publisher left_rect_pub = it.advertise("stereo/left/image_rect", 1);
        image_transport::Publisher right_rect_pub = it.advertise("stereo/right/image_rect", 1);
        image_transport::Publisher disparity_image_pub = it.advertise("stereo/disparity_image", 1);
        image_transport::Publisher depth_pub = it.advertise("stereo/depth", 1);
        ros::Publisher point_cloud_pub = nh.advertise<sensor_msgs::PointCloud2> ("stereo/point_cloud", 1);
        ros::Publisher cam_time_stamp_pub = nh.advertise<sensor_msgs::TimeReference>("stereo/timestamp",1);
        ros::Publisher left_cam_info_pub = nh.advertise<sensor_msgs::CameraInfo>("stereo/left/camera_info", 1);
        ros::Publisher right_cam_info_pub = nh.advertise<sensor_msgs::CameraInfo>("stereo/right/camera_info", 1);
        sensor_msgs::TimeReference time_stamp_msg;
        sensor_msgs::CameraInfo left_info, right_info;

        ROS_INFO("Loading from ROS calibration files");

        // get config from the left, right.yaml in config
        camera_info_manager::CameraInfoManager info_manager(nh);

        info_manager.setCameraName("left");
        info_manager.loadCameraInfo( "package://People_counter/config/left.yaml");
        left_info = info_manager.getCameraInfo();

        info_manager.setCameraName("right");
        info_manager.loadCameraInfo( "package://People_counter/config/right.yaml");
        right_info = info_manager.getCameraInfo();

        left_info.header.frame_id = left_frame_id_;
        right_info.header.frame_id = right_frame_id_;

        /**********************************************************************************************************/
        float scale = 1.f;
        cv::Mat Q = cv::Mat::zeros(4, 4, CV_64F);
        cv::Mat map11, map12, map21, map22;

        cv::Size img_size = cv::Size(ocams->width_, ocams->height_);

        // reading intrinsic parameters
        cv::Mat M1, D1, M2, D2;

        M1 = cv::Mat(3, 3, CV_64F, &left_info.K);
        M2 = cv::Mat(3, 3, CV_64F, &right_info.K);

        D1 = cv::Mat(1, 5, CV_64F, &left_info.D.at(0));
        D2 = cv::Mat(1, 5, CV_64F, &right_info.D.at(0));

        M1 *= scale;
        M2 *= scale;

        cv::Mat R, T, R1, P1, R2, P2;

        R1 = cv::Mat(3, 3, CV_64F, &left_info.R);
        P1 = cv::Mat(3, 4, CV_64F, &left_info.P);
        R2 = cv::Mat(3, 3, CV_64F, &right_info.R);
        P2 = cv::Mat(3, 4, CV_64F, &right_info.P);

        cv::initUndistortRectifyMap(M1, D1, R1, P1, img_size, CV_32FC1, map11, map12);
        cv::initUndistortRectifyMap(M2, D2, R2, P2, img_size, CV_32FC1, map21, map22);

        double Tx = 0.12;   //baseline
        double focal = P2.at<double>(0,0);
        Q.at<double>(0,0) = Q.at<double>(1,1) = 1.0;
        Q.at<double>(0,3) = -P2.at<double>(0,2);
        Q.at<double>(1,3) = -P2.at<double>(1,2);
        Q.at<double>(2,3) = P2.at<double>(0,0);
        Q.at<double>(3,2) = 1.0 / Tx;

        std::cout << Q << std::endl;
        ROS_INFO("Got camera calibration files");

        /******************************************************************************************************************/

        cv::Mat left_raw, right_raw;
        cv::Mat left_rgb, right_rgb;
        cv::Mat left_mono, right_mono;
        cv::Mat left_rect_color, right_rect_color;
        cv::Mat left_rect_mono, right_rect_mono;
        cv::Mat disp16, disp8;

        uint32_t time_stamp, sec, nsec;


        static double depth_up;
        static double depth_mid;
        static double depth_down;
        static float height;
        double height_mid;
        string myText1;
        string myText2;
        string myText3;
        cv::Point myPoint1;
        myPoint1.x = 10;
        myPoint1.y = 15;
        cv::Point myPoint2;
        myPoint2.x = 10;
        myPoint2.y = 30;
        cv::Point myPoint3;
        myPoint3.x = 10;
        myPoint3.y = 45;

        while (ros::ok())
        {
            ros::Time now = ros::Time::now();

            if (!ocams->getImages(left_raw, right_raw, time_stamp)) {
                usleep(10);
                continue;
            } else {
                ROS_INFO_ONCE("Success, found camera_123");
            }



            /****************** Rectification *****************/
            cv::cvtColor(left_raw, left_rgb, CV_BayerGR2RGB);
            cv::cvtColor(right_raw, right_mono, CV_BayerGR2GRAY);

            Rectification(left_rgb, right_mono, map11, map12, map21, map22, left_rect_color, right_rect_mono);
            cv::cvtColor(left_rect_color, left_rect_mono, CV_BGR2GRAY);

            //Rect roi_rect(280,130,150,200);
            Rect roi_rect(roi_x,roi_y,roi_w,roi_h);
            Mat roi_left_rect;
            Mat roi_right_rect;
            imshow("color", left_rect_color);

            roi_left_rect = left_rect_mono(roi_rect);
            roi_right_rect = right_rect_mono(roi_rect);

            // STEREO_BM=0, STEREO_SGBM=1, STEREO_HH=2, STEREO_3WAY=3, STEREO_HH4=4
            stereoMatch(roi_left_rect, roi_right_rect, disp16, state.mode);
            cv::Mat disp32;
            disp16.convertTo(disp32, CV_32F, 1./16);

            /* time stamp publish */
            sec = (uint32_t)time_stamp/1000;
            nsec = (uint32_t)(time_stamp - sec*1000) * 1e6;
            ros::Time measurement_time(sec, nsec);
            ros::Time time_ref(0, 0);
            time_stamp_msg.header.stamp = measurement_time;
            time_stamp_msg.header.frame_id = left_frame_id_;
            time_stamp_msg.time_ref = time_ref;
            cam_time_stamp_pub.publish(time_stamp_msg);

            now = measurement_time;

            if (left_image_pub.getNumSubscribers() > 0) {
                publishImage(left_rgb, left_image_pub, "left_frame", now, sensor_msgs::image_encodings::BGR8);
            }
            if (right_image_pub.getNumSubscribers() > 0) {
                publishImage(right_mono, right_image_pub, "right_frame", now, sensor_msgs::image_encodings::MONO8);
            }
            if (left_cam_info_pub.getNumSubscribers() > 0) {
                publishCamInfo(left_cam_info_pub, left_info, now);
            }
            if (right_cam_info_pub.getNumSubscribers() > 0) {
                publishCamInfo(right_cam_info_pub, right_info, now);
            }
            if (left_rect_pub.getNumSubscribers() > 0) {
                publishImage(left_rect_color, left_rect_pub, "left_frame", now, sensor_msgs::image_encodings::BGR8);
            }
            if (right_rect_pub.getNumSubscribers() > 0) {
                publishImage(right_rect_mono, right_rect_pub, "left_frame", now, sensor_msgs::image_encodings::MONO8);
            }

            if (disparity_image_pub.getNumSubscribers() > 0) {
                disp16.convertTo(disp8, CV_8U, 255/(state.numDisparities*16.));
                //conv_disp16.convertTo(conv_disp8, CV_8U, 255/(state.numDisparities*16.));
                cv::Mat disp_bgr;
                //cv::Mat conv_disp_bgr;
                cv::applyColorMap(disp8, disp_bgr, cv::COLORMAP_JET);
                //cv::applyColorMap(conv_disp8, conv_disp_bgr, cv::COLORMAP_JET);
                publishImage(disp_bgr, disparity_image_pub, "left_frame", now, sensor_msgs::image_encodings::BGR8);
                //publishImage(disp16, disparity_image_pub, "left_frame", now, sensor_msgs::image_encodings::TYPE_16SC1);
            }
            if (depth_pub.getNumSubscribers() > 0) {
                cv::Mat depth32f;
                int min_disparity = 0;
                int max_disparity = 128;

                depth32f = cv::Mat::zeros(disp32.rows, disp32.cols, CV_32F);
                for (int i = 0; i < disp32.rows; i++)
                {
                    for (int j = 0; j < disp16.cols; j++)
                    {
                        float disparity_value = (float)disp32.at<float>(i,j);
                        if (disparity_value > min_disparity && disparity_value < max_disparity)
                        {
                            // baseline * focal / disparity
                            float depth = Tx * focal / disparity_value;
                            depth32f.at<float>(i,j) = depth;
                        }
                    }
                }
                publishDepth(depth32f, depth_pub, "left_frame", now, 0);
            }
            if (point_cloud_pub.getNumSubscribers() > 0) {
                //publishPointCloud(points, left_rect_color, point_cloud_pub);
            }


            if (show_image_) {
                disp16.convertTo(disp8, CV_8U, 255/(state.numDisparities*16.));
                cv::applyColorMap(disp8, dispbgr, cv::COLORMAP_JET);

                double T = 0.12;   //baseline
                double F = P2.at<double>(0,0);

                height_mid = cal_minvalue(disp32, 0, 0, 129, 199, T, F);
                depth_mid = 0.5 * depth_mid + 0.5 * height_mid;

                //depth_up = 0.5 * depth_up + 0.5 * cal_minvalue(disp32,0,10,149,30,T,F);
                //depth_down = 0.5 * depth_down + 0.5 * cal_minvalue(disp32, 0,170,149,180,T,F);
		depth_up = 0.5 * depth_up + 0.5 * cal_minvalue(disp32,detect_upline_x1 ,detect_upline_y1,detect_upline_x2,detect_upline_y2,T,F);
                depth_down = 0.5 * depth_down + 0.5 * cal_minvalue(disp32, detect_downline_x1,detect_downline_y1,detect_downline_x2,detect_downline_y2,T,F);

                if( depth_up > 0.01 )
                {
                    status_ent = true;
                    check_status_ent = true;
                }
                else
                {
                    status_ent = false;
                }

                if(check_status_ent) //enter the Enter Line
                {
                    if(!status_ent) //pass the Enter Line
                    {
                        //printf("Passed Enter Line\n");
                        if(pass_status_out) //check the sequence
                        {
                            //printf("--- Enter ---\n");
                            count_people_ent++;
                            pass_status_ent = false;
                            pass_status_out = false;
                            check_status_ent = false;
                            check_height = true;

                        }
                        else // pass the enter line first
                        {
                            check_status_ent = false; //pause the Check sequence whether person enter
                            pass_status_ent = true;   //set the flag
                        }
                    }
                    else
                    {
                        //count time and change value of 'check_status_ent' to false
                    }
                }
                if( depth_down > 0.01 )
                {
                    status_out = true;
                    check_status_out = true;
                }
                else
                {
                    status_out = false;
                }
                if(check_status_out)
                {
                    if(!status_out)
                    {
                        if(pass_status_ent) // pass the enter line first
                        {
                            count_people_out++;
                            pass_status_ent = false;
                            pass_status_out = false;
                            check_status_out = false;
                            check_height = true;
                        }
                        else
                        {
                            check_status_out = false;
                            pass_status_out = true;
                        }
                    }
                }

                if( depth_mid < 0.001 )
                {
                    status_ent = false;
                    status_out = false;
                    pass_status_ent = false;
                    pass_status_out = false;
                    check_status_out = false;
                    check_status_ent = false;
                }
                if( min_height == 0.0){
                    if( (height_mid < 1.5) && (height_mid > 0.7) )
                    {
                        min_height = height_mid;
                    }
                }
                if(	min_height > height_mid)
                {
                    if( (height_mid < 1.5) && (height_mid > 0.7) )
                    {
                        min_height = height_mid;
                    }
                }
                if(check_height)
                {
                    height = ceiling_height - min_height;
                    height = floorf(height * 100) / 100;
                    check_height = false;
                    min_height = 0.0;
                }

                myText1 = "Enter : " + std::to_string(count_people_ent);
                myText2 = "Exit  : " + std::to_string(count_people_out);
                myText3 = "Height  : " + std::to_string(height) + "m";
                cv::putText( dispbgr, myText1, myPoint1, 2, 0.5, Scalar::all(255) );
                cv::putText( dispbgr, myText2, myPoint2, 2, 0.5, Scalar::all(255) );
                cv::putText( dispbgr, myText3, myPoint3, 2, 0.5, Scalar::all(255) );
                cv::namedWindow("disp",0);
                cv::imshow("disp", dispbgr);
                cv::waitKey(1);
            }

        }
    }

    float cal_minvalue(Mat& disp32, int x1, int y1, int x2 , int y2 , double T, double F)
    {
        float min = 0.0;
        float depth_mid = 0.0;
        for(int j= y1 ; j < y2 ; j++)
        {
            for(int i = x1 ; i < x2 ; i++) //140
            {
                depth_mid = T * F / (double)disp32.at<float>(j,i);
                if( (depth_mid < 1.5) && (depth_mid > 0.7) )
                {
                    if (min == 0.0)
                        min = depth_mid;
                    if(	min > depth_mid)
                    {
                        min = depth_mid;
                    }

                }
            }
        }
        return min;
    }

    void ctlExposure(int exposure)
    {
        int exposure_ = exposure;
        int gain_ = 150;
        int wb_blue_ = 200;
        int wb_red_ = 160;
        bool autoexposure_= false;
        ocams->uvc_control(exposure_ ,  gain_ , wb_blue_ , wb_red_ , autoexposure_);
    }

    void callback(People_counter::camConfig &config, uint32_t level) {
        //        ROS_INFO("exposure:%d, gain:%d, blue:%d, red:%d, ae:%d", config.exposure, config.gain, config.wb_blue, config.wb_red, config.auto_exposure);
        ocams->uvc_control(config.exposure, config.gain, config.wb_blue, config.wb_red, config.auto_exposure);

        state.mode = (MatchingAlg)config.stereo_algorithm;
        state.preFilterType = config.prefilter_type;
        if(config.prefilter_size % 2 == 0)
            config.prefilter_size--;
        state.preFilterSize = config.prefilter_size;
        state.preFilterCap = config.prefilter_cap;
        if( (config.stereo_algorithm == 0) && (config.correlation_window_size<5) )
        {
            config.correlation_window_size = 5;
        }
        if(config.correlation_window_size % 2 == 0)
        {
            config.correlation_window_size--;
        }
        state.SADWindowSize = config.correlation_window_size;
        state.minDisparity = config.min_disparity;
        if(config.disparity_range % 16 != 0)
            config.disparity_range = config.disparity_range/16*16;
        state.numDisparities = config.disparity_range;
        state.textureThreshold = config.texture_threshold;
        state.uniquenessRatio = config.uniqueness_ratio;
        state.speckleRange = config.speckle_range;
        state.speckleWindowSize = config.speckle_size;
        state.disp12MaxDiff = config.disp12MaxDiff;
        state.p1 = config.P1;
        state.p2 = config.P2;
    }


public:
    /**
         * @brief      { function_description }
         *
         * @param[in]  resolution  The resolution
         * @param[in]  frame_rate  The frame rate
     */
    oCamStereoROS() {
        ros::NodeHandle priv_nh("~");

        /* default parameters */
        resolution_ = 2;
        frame_rate_ = 30.0;
        exposure_ = 100;
        gain_ = 150;
        wb_blue_ = 200;
        wb_red_ = 160;
        autoexposure_= false;
        left_frame_id_ = "left_camera";
        right_frame_id_ = "right_camera";
        show_image_ = true;
        roi_x = 0;
        roi_y = 0;
        roi_h = 0;
        roi_w = 0;
        ceiling_height = 0.0f;

        detect_upline_x1 = 0;
        detect_upline_y1 = 10;
        detect_upline_x2 = 149;
        detect_upline_y2 = 30;

        detect_downline_x1 = 0;
        detect_downline_y1 = 170;
        detect_downline_x2 = 149;
        detect_downline_y2 = 180;

        /* get parameters */
        priv_nh.getParam("resolution", resolution_);
        priv_nh.getParam("frame_rate", frame_rate_);
        priv_nh.getParam("exposure", exposure_);
        priv_nh.getParam("gain", gain_);
        priv_nh.getParam("wb_blue", wb_blue_);
        priv_nh.getParam("wb_red", wb_red_);
        priv_nh.getParam("left_frame_id", left_frame_id_);
        priv_nh.getParam("right_frame_id", right_frame_id_);
        priv_nh.getParam("show_image", show_image_);
        priv_nh.getParam("auto_exposure", autoexposure_);
        priv_nh.getParam("ROI_x", roi_x);
        priv_nh.getParam("ROI_y", roi_y);
        priv_nh.getParam("ROI_width", roi_w);
        priv_nh.getParam("ROI_height", roi_h);
        priv_nh.getParam("ceiling_height", ceiling_height);


        priv_nh.getParam("detect_upline_x1"   , detect_upline_x1);
        priv_nh.getParam("detect_upline_y1"   , detect_upline_y1);
        priv_nh.getParam("detect_upline_x2"   , detect_upline_x2);
        priv_nh.getParam("detect_upline_y2"   , detect_upline_y2);
        priv_nh.getParam("detect_downline_x1" , detect_downline_x1);
        priv_nh.getParam("detect_downline_y1" , detect_downline_y1);
        priv_nh.getParam("detect_downline_x2" , detect_downline_x2);
        priv_nh.getParam("detect_downline_y2" , detect_downline_y2);


        /* initialize the camera */
        ocams = new StereoCamera(resolution_, frame_rate_);
        ocams->uvc_control(exposure_, gain_, wb_blue_, wb_red_, autoexposure_);
        ROS_INFO("Initialized the camera");

        // thread

        boost::shared_ptr<boost::thread> device_poll_thread;
        device_poll_thread = boost::shared_ptr<boost::thread>(new boost::thread(&oCamStereoROS::device_poll, this));
    }

    ~oCamStereoROS() {
        delete ocams;
        delete IMU;
    }
};

int main (int argc, char **argv)
{
    ros::init(argc, argv, "People_counter");

    ros::NodeHandle nh;
    ros::NodeHandle priv_nh("~");

    std::string port = std::string("/dev/ttyACM0");
    int baud_rate    = 115200;
    std::string imu_mode = std::string("AMGQUA");

    ros::param::get("~port", port);
    ros::param::get("~imu_mode", imu_mode);
    ros::param::get("~baud_rate", baud_rate);

    MyAhrsDriverForROS sensor(port, baud_rate);
    if(sensor.initialize(imu_mode) == false)
    {
        ROS_ERROR("%s\n", "IMU initialize false!\r\n oCamS-1CGN-U sends IMU data through Virtual COM port.\r\n \
                  So, user needs to write following rules into udev rule file like below.\r\n \
                  -------------------------------------------------------------------------------\r\n \
                  $ sudo vi /etc/udev/rules.d/99-ttyacms.rules\r\n \
                  ATTRS{idVendor}==\"04b4\" ATTRS{idProduct}==\"00f9\", MODE=\"0666\", ENV{ID_MM_DEVICE_IGNORE}=\"1\"\r\n \
                  ATTRS{idVendor}==\"04b4\" ATTRS{idProduct}==\"00f8\", MODE=\"0666\", ENV{ID_MM_DEVICE_IGNORE}=\"1\"\r\n \
                  $ sudo udevadm control -R\r\n \
                  -------------------------------------------------------------------------------\r\n");
    }
    oCamStereoROS ocams_ros;

    ros::spin();

    return 0;
}


