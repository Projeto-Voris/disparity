#include "disparity.hpp"


#include<string>

using std::placeholders::_1;

int numDisparities = 12*16;
int blockSize = 75;
int preFilterType = 0;
int preFilterSize = 25;
int preFilterCap = 15;
int minDisparity = 1;
int textureThreshold = 61;
int uniquenessRatio = 0;
int speckleRange = 50;
int speckleWindowSize = 38;
int disp12MaxDiff = 20;
float lambda = 8000;
float sigma = 1.5;

cv::Ptr<cv::StereoBM> stereo = cv::StereoBM::create();
cv::Ptr<cv::ximgproc::DisparityWLSFilter> wls_filter = cv::ximgproc::createDisparityWLSFilter(stereo);
cv::Ptr<cv::StereoMatcher> right_matcher = cv::ximgproc::createRightMatcher(stereo);


DisparityNode::DisparityNode(sensor_msgs::msg::CameraInfo infoL, sensor_msgs::msg::CameraInfo infoR): Node("node")
{
    std::string left_image_topic = "/left/image_raw";
    std::string right_image_topic = "/right/image_raw";
    std::string params_topic = "/params";

    left_camera_info = infoL;
    right_camera_info = infoR;

    left_sub = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, left_image_topic);
    right_sub = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, right_image_topic);


    CalculateRectificationRemaps();

    syncApproximate = std::make_shared<message_filters::Synchronizer<approximate_sync_policy>> (approximate_sync_policy(10), *left_sub, *right_sub);
    syncApproximate->registerCallback(std::bind(&DisparityNode::GrabStereo, this, std::placeholders::_1, std::placeholders::_2));

    params_sub = create_subscription<std_msgs::msg::Int16MultiArray>(params_topic, 10, std::bind(&DisparityNode::UpdateParameters, this, _1));


    disparity_publisher = this->create_publisher<sensor_msgs::msg::Image>("disparity_image",10);

    rect_left_publisher = this->create_publisher<sensor_msgs::msg::Image>("rect_left_image",10);
    rect_right_publisher = this->create_publisher<sensor_msgs::msg::Image>("rect_right_image",10);
}

void DisparityNode::GrabStereo(const ImageMsg::ConstSharedPtr msgLeft, const ImageMsg::ConstSharedPtr msgRight)
{
    try
    {
         cv_ptrLeft = cv_bridge::toCvShare(msgLeft, "mono8"); 
         cv_ptrRight = cv_bridge::toCvShare(msgRight, "mono8");
    }
    catch (cv_bridge::Exception& e)
    {
        RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
        return;
    }

    auto imgmsg = sensor_msgs::msg::Image();

    

    cv::Mat imgL = cv_ptrLeft->image;
    cv::Mat imgR = cv_ptrRight->image;
    cv::Mat disp, disparity, raw_right_disparity_map, right_disparity;
    cv::Mat filtered_disparity_map, filtered_disparity_map_16u;

    RectifyImages(imgL, imgR);

   
    stereo->compute(rectImgL, rectImgR, disp);
    disp.convertTo(disparity,CV_8UC1, 1);

    
    right_matcher->compute( rectImgR,rectImgL, raw_right_disparity_map);


    wls_filter->filter(disp,
                        rectImgL,
                        filtered_disparity_map,
                        raw_right_disparity_map);
    raw_right_disparity_map.convertTo(right_disparity, CV_32FC1, 1);
    filtered_disparity_map.convertTo(filtered_disparity_map_16u, CV_8UC1, 0.2);

    cv_bridge::CvImage(std_msgs::msg::Header(), "mono8", filtered_disparity_map_16u).toImageMsg(imgmsg);
    disparity_publisher->publish(imgmsg);

    
}

void DisparityNode::UpdateParameters(const std_msgs::msg::Int16MultiArray::ConstSharedPtr params_message)
{
    RCLCPP_INFO(this->get_logger(), "Received: %d", params_message->data[0]);
    stereo->setPreFilterCap(params_message->data[0]); //1 - 63
    stereo->setPreFilterSize(params_message->data[1]); // 5 - 255 impar
    stereo->setPreFilterType(params_message->data[2]);

    stereo->setTextureThreshold(params_message->data[3]); 
    stereo->setUniquenessRatio(params_message->data[4]);

    stereo->setNumDisparities(params_message->data[5]); // positivo %16 == 0
    stereo->setBlockSize(params_message->data[6]); // 5- 255 impar
    stereo->setSpeckleRange(params_message->data[7]);

    stereo->setSpeckleWindowSize(params_message->data[8]);
    stereo->setDisp12MaxDiff(params_message->data[9]);
    stereo->setMinDisparity(params_message->data[10]);

    wls_filter->setLambda(params_message->data[11]);
    wls_filter->setSigmaColor(params_message->data[12]);

    minDisparity = params_message->data[10];
    numDisparities = params_message->data[5];
}

void DisparityNode::RectifyImages(cv::Mat imgL, cv::Mat imgR)
{   
    cv::remap(imgL, rectImgL, left_map1, left_map2, cv::INTER_LANCZOS4);
    cv::remap(imgR, rectImgR, right_map1, right_map2, cv::INTER_LANCZOS4);

    auto leftimgmsg = sensor_msgs::msg::Image();
    auto rightimgmsg = sensor_msgs::msg::Image();

    /*cv_bridge::CvImage(std_msgs::msg::Header(), "mono8", rectImgL).toImageMsg(leftimgmsg);
    //rect_left_publisher->publish(leftimgmsg);

    cv_bridge::CvImage(std_msgs::msg::Header(), "mono8", rectImgR).toImageMsg(rightimgmsg);
    //rect_right_publisher->publish(rightimgmsg);*/
}

void DisparityNode::CalculateRectificationRemaps()
{
    cv::Mat intrinsics_left(3, 3, cv::DataType<double>::type);
    cv::Mat dist_coeffs_left(5, 1, cv::DataType<double>::type);

    cv::Mat intrinsics_right(3, 3, cv::DataType<double>::type);
    cv::Mat dist_coeffs_right(5, 1, cv::DataType<double>::type);
    
    cv::Mat rotation(3, 3, cv::DataType<double>::type);
    cv::Mat translation(3, 1, cv::DataType<double>::type);

    cv::Mat R1,R2,P1,P2,Q;

    cv::Size siz;

    intrinsics_left.at<double>(0,0) = left_camera_info.k[0]; //fx
    intrinsics_left.at<double>(0,2) = left_camera_info.k[2]; //cx
    intrinsics_left.at<double>(1,1) = left_camera_info.k[4]; //fy
    intrinsics_left.at<double>(1,2) = left_camera_info.k[5]; //cy
    intrinsics_left.at<double>(2,2) = 1;
    
    dist_coeffs_left.at<double>(0) = left_camera_info.d[0]; //k1
    dist_coeffs_left.at<double>(1) = left_camera_info.d[1]; //k2
    dist_coeffs_left.at<double>(2) = left_camera_info.d[3]; //p1
    dist_coeffs_left.at<double>(3) = left_camera_info.d[4]; //p2
    dist_coeffs_left.at<double>(4) = left_camera_info.d[2]; //k3

    intrinsics_right.at<double>(0,0) = right_camera_info.k[0];
    intrinsics_right.at<double>(0,2) = right_camera_info.k[2];
    intrinsics_right.at<double>(1,1) = right_camera_info.k[4];
    intrinsics_right.at<double>(1,2) = right_camera_info.k[5];
    intrinsics_right.at<double>(2,2) = 1;
    
    dist_coeffs_right.at<double>(0) = right_camera_info.d[0];
    dist_coeffs_right.at<double>(1) = right_camera_info.d[1];
    dist_coeffs_right.at<double>(2) = right_camera_info.d[3];
    dist_coeffs_right.at<double>(3) = right_camera_info.d[4];
    dist_coeffs_right.at<double>(4) = right_camera_info.d[2];

    siz.width = left_camera_info.width;
    siz.height = left_camera_info.height;

    rotation.at<double>(0, 0) = left_camera_info.r[0];
    rotation.at<double>(0, 1) = left_camera_info.r[1];
    rotation.at<double>(0, 2) = left_camera_info.r[2];
    rotation.at<double>(1, 0) = left_camera_info.r[3];
    rotation.at<double>(1, 1) = left_camera_info.r[4];
    rotation.at<double>(1, 2) = left_camera_info.r[5];
    rotation.at<double>(2, 0) = left_camera_info.r[6];
    rotation.at<double>(2, 1) = left_camera_info.r[7];
    rotation.at<double>(2, 2) = left_camera_info.r[8];

    translation.at<double>(0) = left_camera_info.p[3];
    translation.at<double>(1) = left_camera_info.p[7];
    translation.at<double>(2) = left_camera_info.p[11];


    cv::stereoRectify(intrinsics_left, dist_coeffs_left, intrinsics_right, dist_coeffs_right, siz, rotation, translation, R1, R2, P1, P2, Q);

    cv::initUndistortRectifyMap(intrinsics_left, dist_coeffs_left, R1, P1,siz, CV_32FC1,left_map1, left_map2);
    cv::initUndistortRectifyMap(intrinsics_right, dist_coeffs_right, R2, P2,siz, CV_32FC1,right_map1, right_map2);

}