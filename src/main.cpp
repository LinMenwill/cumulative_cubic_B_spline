#define BACKWARD_HAS_DW 1
#include <backward.hpp>
namespace backward
{
backward::SignalHandling sh;
} // namespace backward

#include <cstdio>
#include <string>
#include <queue>
#include <vector>
#include <eigen3/Eigen/Dense>
#include <ros/ros.h>
#include <ros/package.h>
#include <sophus/se3.hpp>
#include <boost/thread.hpp>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseStamped.h>
#include <stereo_msgs/DisparityImage.h>
#include <sensor_msgs/CameraInfo.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>

#include "utils.h"
#include "ceres_solve.h"
#include "data_structure.h"

using namespace std;

Graph graph;
CALI_PARA cali;

queue<sensor_msgs::Image> img1_buffer;
queue<stereo_msgs::DisparityImage> disp_buffer;
queue<sensor_msgs::Imu> imu_buffer;
boost::mutex mtx;
Eigen::Matrix4d B;
FILE *solve_file;  // Solved B-spline: ts p \theta
FILE *solve_omega_file;  // Solved B-spline': ts \omega
FILE *solve_vel_file;  // Solved B-spline': ts vel
FILE *solve_acc_file;  // Solved B-spline'': ts acc
FILE *debug_imu_file;  // IMU: ts acc \omega
double deltaT;       // eg: 20HZ img (50ms) -> 0.05
nav_msgs::Path path;
Eigen::Vector3d g0,initial_omega_bias;
ros::Time start_time_stamp;
double last_imu_stamp;
int key_frame_no;
int calc_level;

void ros_pub_points(State& state, ros::Publisher& pub_pc2, ros::Time& ros_stamp)
{
    int w,h,level;
    level = 0;
    w = state.depth[level].cols();
    h = state.depth[level].rows();
    

    sensor_msgs::PointCloud2 pc2_msg;
    pc2_msg.header.stamp = ros_stamp;
    pc2_msg.header.frame_id = "ref_frame";
    pc2_msg.height = h;
    pc2_msg.width = w;
    pc2_msg.fields.resize(4);
    pc2_msg.fields[0].name = "x";
    pc2_msg.fields[0].offset = 0;
    pc2_msg.fields[0].count = 1;
    pc2_msg.fields[0].datatype = sensor_msgs::PointField::FLOAT32;
    pc2_msg.fields[1].name = "y";
    pc2_msg.fields[1].offset = 4;
    pc2_msg.fields[1].count = 1;
    pc2_msg.fields[1].datatype = sensor_msgs::PointField::FLOAT32;
    pc2_msg.fields[2].name = "z";
    pc2_msg.fields[2].offset = 8;
    pc2_msg.fields[2].count = 1;
    pc2_msg.fields[2].datatype = sensor_msgs::PointField::FLOAT32;
    pc2_msg.fields[3].name = "rgb";
    pc2_msg.fields[3].offset = 12;
    pc2_msg.fields[3].count = 1;
    pc2_msg.fields[3].datatype = sensor_msgs::PointField::FLOAT32;
    pc2_msg.is_bigendian = false;
    pc2_msg.point_step = sizeof(float) * 4;
    pc2_msg.row_step = pc2_msg.point_step * pc2_msg.width;
    pc2_msg.data.resize(pc2_msg.row_step * pc2_msg.height);
    pc2_msg.is_dense = true;

    int i = 0;
    for (int u=0;u<h;u++)
        for (int v=0;v<w;v++,i++)
        {
            Eigen::Vector3d p_ref;
            double lambda;
            lambda = state.depth[level](u,v);

            if (double_equ_check(lambda,0.0,DOUBLE_EPS)<=0) // no depth
                lambda = 0.0;

            p_ref = Eigen::Vector3d( (v-cali.cx[level])/cali.fx[level],
                                     (u-cali.cy[level])/cali.fy[level],
                                      1.0) * lambda; // (x,y,z)^{ref}

            float x = p_ref(0);
            float y = p_ref(1);
            float z = p_ref(2);
            uchar g = (uchar)state.img_data[level](u,v);
            int32_t rgb = (g << 16) | (g << 8) | g;

            memcpy(&pc2_msg.data[i * pc2_msg.point_step + 0], &x, sizeof(float));
            memcpy(&pc2_msg.data[i * pc2_msg.point_step + 4], &y, sizeof(float));
            memcpy(&pc2_msg.data[i * pc2_msg.point_step + 8], &z, sizeof(float));
            memcpy(&pc2_msg.data[i * pc2_msg.point_step + 12], &rgb, sizeof(int32_t));
        }
    pub_pc2.publish(pc2_msg);
}

// pyr_down: (img, depth)
void pyr_down(State& state,int level)
{
    if (level==0) return;

    int w,h;
    w = cali.width[level];
    h = cali.height[level];
    state.img_data[level] = Eigen::MatrixXd::Zero(h,w);
    state.depth[level] = Eigen::MatrixXd::Zero(h,w);
    printf("pyrdown: %d x %d\n",w,h);

    for (int u=0;u<w;u++)
        for (int v=0;v<h;v++)
        {
            int u1,v1;
            u1 = u<<1;
            v1 = v<<1;

            double t;
            t = ( state.img_data[level-1](v1,u1)
                + state.img_data[level-1](v1,u1+1)
                + state.img_data[level-1](v1+1,u1)
                + state.img_data[level-1](v1+1,u1+1))
                / 4.0;
            state.img_data[level](v,u) = t;

            t = ( state.depth[level-1](v1,u1)
                + state.depth[level-1](v1,u1+1)
                + state.depth[level-1](v1+1,u1)
                + state.depth[level-1](v1+1,u1+1))
                    / 4.0;
            state.depth[level](v,u) = t;
        }
}

void vision_callback(
    const sensor_msgs::ImageConstPtr& msg_img_l,
    const stereo_msgs::DisparityImageConstPtr& msg_disp)
{
    ros::Duration img_delay(cali.exposure_time/2.0);  // visensor's img stamp is the middle of exposure period
    sensor_msgs::Image img_msg = *msg_img_l;
    stereo_msgs::DisparityImage disp_msg = *msg_disp;
    img_msg.header.stamp = img_msg.header.stamp + img_delay;
    disp_msg.header.stamp = disp_msg.header.stamp + img_delay;
    
    mtx.lock();
    img1_buffer.push(img_msg);
    disp_buffer.push(disp_msg);
    mtx.unlock();
}

void imu_callback(const sensor_msgs::ImuConstPtr& msg)
{
    mtx.lock();
    imu_buffer.push(*msg);
    mtx.unlock();
}

void spin_thread()
{
    while (ros::ok())
    {
        ros::spinOnce();
    }
}

void init()
{
    solve_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/solve_pose.txt","w");
    solve_omega_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/solve_omega.txt","w");
    solve_vel_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/solve_vel.txt","w");
    solve_acc_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/solve_acc.txt","w");
    debug_imu_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/debug_imu.txt","w");
    // ---- construct B ----
    B.setZero();

    B(0,0) = 6.0;

    B(1,0) = 5.0;
    B(1,1) = 3.0;
    B(1,2) = -3.0;
    B(1,3) = 1.0;

    B(2,0) = 1.0;
    B(2,1) = 3.0;
    B(2,2) = 3.0;
    B(2,3) = -2.0;

    B(3,3) = 1.0;

    Eigen::Matrix4d tmp_B;
    tmp_B = 1.0/6.0 * B;
    B = tmp_B;
    cout << B << endl;
    // ----------

    // === read config ===
    ros::NodeHandle nh("~");

    if (!cali.init(nh))
    {
        ROS_ERROR("cali init error!");
        exit(1);
    }
    if (
        !nh.getParam("calc_level",calc_level) ||
        !nh.getParam("exposure_time",cali.exposure_time) ||
        !nh.getParam("img_hz",cali.img_hz)
       )
        {
            puts("Can not read param from .launch !");
        }
    deltaT = 1.0/cali.img_hz;
    cali.view();

    while (!imu_buffer.empty()) imu_buffer.pop();
    while (!img1_buffer.empty()) img1_buffer.pop();
    while (!disp_buffer.empty()) disp_buffer.pop();

    path.header.frame_id = "world";
    path.poses.clear();
}

void get_stationary_imu()
{
    ros::Time imu_stamp,img_stamp;

    // ensure there is IMG (& disp) in buffer
    mtx.lock();
    while (img1_buffer.empty())
    {
        mtx.unlock();
        usleep(1000);
        mtx.lock();
    }
    img_stamp = img1_buffer.front().header.stamp;
    mtx.unlock();

    // ensure there is IMU in the buffer, and this IMU stamp < IMG stamp
    mtx.lock();
    while (imu_buffer.empty() || imu_buffer.front().header.stamp < img_stamp)
    {
        if (!imu_buffer.empty()) imu_buffer.pop();
        mtx.unlock();
        usleep(1000);
        mtx.lock();
    }
    imu_stamp = imu_buffer.front().header.stamp;
    mtx.unlock();

    // ========================================
    //  drop first img & disp for computing g0
    // ========================================
    mtx.lock();
    img1_buffer.pop();
    disp_buffer.pop();
    while (img1_buffer.empty())
    {
        mtx.unlock();
        usleep(1000);
        mtx.lock();
    }
    img_stamp = img1_buffer.front().header.stamp;
    mtx.unlock();
    // -----
    g0.setZero();
    initial_omega_bias.setZero();
    int cnt = 0;
    mtx.lock();
    while (imu_buffer.empty() || imu_buffer.front().header.stamp < img_stamp)
    {
        while (!imu_buffer.empty() && imu_buffer.front().header.stamp < img_stamp)
        {
            cnt++;
            g0(0) = g0(0) + imu_buffer.front().linear_acceleration.x;
            g0(1) = g0(1) + imu_buffer.front().linear_acceleration.y;
            g0(2) = g0(2) + imu_buffer.front().linear_acceleration.z;
            initial_omega_bias(0) = initial_omega_bias(0) + imu_buffer.front().angular_velocity.x;
            initial_omega_bias(1) = initial_omega_bias(1) + imu_buffer.front().angular_velocity.y;
            initial_omega_bias(2) = initial_omega_bias(2) + imu_buffer.front().angular_velocity.z;
            imu_buffer.pop();
        }
        mtx.unlock();
        usleep(1000);
        mtx.lock();
    }
    mtx.unlock();
    g0 = g0 / cnt;
    initial_omega_bias = initial_omega_bias / cnt;

    // ===== output g0, initial_omega_bias, cnt =====
    cout << "g0: " << g0.transpose() << endl;
    cout << "initial_omega_bias: " << initial_omega_bias.transpose() << endl;
    cout << "cnt: " << cnt << endl;
}

void load_img_disp(State& state, cv::Mat &img)
{
    // load cv_img into Eigen matrix
    state.img_data[0] = Eigen::MatrixXd::Zero(cali.height[0],cali.width[0]);
    for (int u=0;u<img.rows;u++)
        for (int v=0;v<img.cols;v++)
            state.img_data[0](u,v) = img.at<uchar>(u,v);

    for (int level=0;level<PYRDOWN_LEVEL;level++)
    {
        pyr_down(state,level); // also pyr_down depth
    }
}

void real_data(State& state)
{
    cv::Mat img,disp;

    // get IMG & disp
    img = cv_bridge::toCvCopy(img1_buffer.front())->image;
    disp = cv_bridge::toCvCopy(disp_buffer.front().image)->image;
    state.ros_stamp = img1_buffer.front().header.stamp;
    state.stamp = (state.ros_stamp - start_time_stamp).toSec();

    img1_buffer.pop();
    disp_buffer.pop();

    int depth_cnt;
    depth_cnt = cal_depth_img(disp,state.depth[0],cali.baseline,cali.fx[0]);  //float
    load_img_disp(state,img);

    if (graph.state.size()==0) // first state, only get IMG & disp & set p=(0,0,0)
    {
        state.p.setZero();
        state.q.setIdentity();
        state.v.setZero();
        last_imu_stamp = 0.0;
        key_frame_no = 0;
    }
    else
    {
        // ==== IMU integrate ====
        int last_no = graph.state.size()-1;  // cur state not yet pushed

        double &DeltaT = state.DeltaT;
        Eigen::Matrix3d &R_k1_k = state.R_k1_k;
        Eigen::Vector3d &alpha = state.alpha;
        Eigen::Vector3d &beta = state.beta;
        Eigen::Vector3d &pk1 = state.p;
        Eigen::Vector3d &vk1 = state.v;
        Eigen::Quaterniond &qk1 = state.q;

        R_k1_k.setIdentity();
        alpha.setZero();
        beta.setZero();
        DeltaT = 0.0;

        while (!imu_buffer.empty() && imu_buffer.front().header.stamp < state.ros_stamp)
        {
            sensor_msgs::Imu imu_msg;
            imu_msg = imu_buffer.front();
            imu_buffer.pop();

            double imu_t = (imu_msg.header.stamp - start_time_stamp).toSec();

            double dt = imu_t - last_imu_stamp;
            DeltaT += dt;
            Eigen::Vector3d a,w;

            a(0) = imu_msg.linear_acceleration.x;
            a(1) = imu_msg.linear_acceleration.y;
            a(2) = imu_msg.linear_acceleration.z;
            w(0) = imu_msg.angular_velocity.x - initial_omega_bias(0);
            w(1) = imu_msg.angular_velocity.y - initial_omega_bias(1);
            w(2) = imu_msg.angular_velocity.z - initial_omega_bias(2);

            Eigen::Quaterniond dq;
            dq.x() = w(0)*dt*0.5;
            dq.y() = w(1)*dt*0.5;
            dq.z() = w(2)*dt*0.5;
            dq.w() = sqrt(1.0 - (dq.x()*dq.x()) - (dq.y()*dq.y()) - (dq.z()*dq.z()) );
            dq.normalize();

            Eigen::Matrix3d dR(dq);
            R_k1_k = R_k1_k * dR;

            // --- remove g0:   R_t^0 = R_k^0 * R_t^k ---
            Eigen::Matrix3d R_t_0;
            R_t_0 = graph.state[last_no].q.toRotationMatrix() * R_k1_k;
            a = a - R_t_0.transpose() * g0;
            // -------

            // ---- transform a,w to camera frame ----
            a = cali.R_I_2_C * a + cali.T_I_2_C;
            w = cali.R_I_2_C * w + cali.T_I_2_C;
            // -----

            fprintf(debug_imu_file,"%lf %lf %lf %lf %lf %lf %lf\n",
                                    imu_t,
                                    a(0),a(1),a(2),
                                    w(0),w(1),w(2)
                   );
            state.imu_data.push_back(IMU_DATA(imu_t,a,w));

            alpha += beta*dt + 0.5*R_k1_k*a*dt*dt;
            beta += R_k1_k*a*dt;

            last_imu_stamp = imu_t;
        }
        printf("DeltaT = %lf\n",DeltaT);
        pk1 = graph.state[last_no].p 
            + graph.state[last_no].q.toRotationMatrix()*graph.state[last_no].v*DeltaT
            + graph.state[last_no].q.toRotationMatrix()*alpha; // g0 has already removed
        vk1 = R_k1_k.transpose()
            * (graph.state[last_no].v + beta);  // g0 has already removed
        qk1 = Eigen::Quaterniond(graph.state[last_no].q.toRotationMatrix() * R_k1_k);
        qk1.normalize();
    }
}

// B-spline can only update (head+2) velocity, so need to re-propogate (head+3)'s p,v,q
void update_state(int head)
{
    int last_no = head+2;
    double &DeltaT = graph.state[head+3].DeltaT;
    Eigen::Matrix3d &R_k1_k = graph.state[head+3].R_k1_k;
    Eigen::Vector3d &alpha = graph.state[head+3].alpha;
    Eigen::Vector3d &beta = graph.state[head+3].beta;
    Eigen::Vector3d &pk1 = graph.state[head+3].p;
    Eigen::Vector3d &vk1 = graph.state[head+3].v;
    Eigen::Quaterniond &qk1 = graph.state[head+3].q;

    pk1 = graph.state[last_no].p 
        + graph.state[last_no].q.toRotationMatrix()*graph.state[last_no].v*DeltaT
        + graph.state[last_no].q.toRotationMatrix()*alpha; // g0 has already removed
    vk1 = R_k1_k.transpose()
        * (graph.state[last_no].v + beta);  // g0 has already removed
    qk1 = Eigen::Quaterniond(graph.state[last_no].q.toRotationMatrix() * R_k1_k);
    qk1.normalize();
}

// ensure there is enough msg
void process()
{
    start_time_stamp = img1_buffer.front().header.stamp;  // ensure img1_buffer & disp_buffer are totally the same

    while (!imu_buffer.empty() && imu_buffer.front().header.stamp < start_time_stamp)
    {
        imu_buffer.pop();
    }

    for (int i=0;i<3;i++)   // add first 3 states
    {
        State state;
        real_data(state);
        graph.state.push_back(state);
    }
    int head = 0;
    while (img1_buffer.empty() || disp_buffer.empty() || imu_buffer.empty())
    {
        State state;
        real_data(state);
        graph.state.push_back(state);
        ceres_process(head);
        update_state(head);  // B-spline can only update (head+2) velocity, so need to re-propogate (head+3)'s p,v,q
        head++;
    }
}

int main(int argc, char **argv)
{
    ros::init(argc,argv,"cumulative_cubic_B_spline");
    ros::NodeHandle nh("~");

    ros::Publisher pub_path = nh.advertise<nav_msgs::Path>("path",1000);
    ros::Publisher pub_img = nh.advertise<sensor_msgs::Image>("image_grey",1000);  // cur frame
    ros::Publisher pub_img_info = nh.advertise<sensor_msgs::CameraInfo>("camera_info",1000);  // cur frame's caminfo
    ros::Publisher pub_depth = nh.advertise<sensor_msgs::Image>("depth",1000);  // cur depth
    ros::Publisher pub_depth_info = nh.advertise<sensor_msgs::CameraInfo>("depth_info",1000);  // cur frame's caminfo
    ros::Publisher pub_pc2 = nh.advertise<sensor_msgs::PointCloud2>("pc2",1000);  // cur frame's point cloud
    ros::Publisher pub_pixel_with_depth = nh.advertise<sensor_msgs::Image>("pixel_with_depth",1000);  // key frame with depth (green pixel)
    ros::Publisher pub_keyframe = nh.advertise<sensor_msgs::Image>("key_frame",1000);  // key frame
    ros::Publisher pub_estimated_img = nh.advertise<sensor_msgs::Image>("est_img",1000);  // est cur frame

    init();

    // ---- sync subscribe ----
    ros::Subscriber sub_imu = nh.subscribe("/imu0",1000,imu_callback);  // 200HZ (5ms)
    message_filters::Subscriber<sensor_msgs::Image> sub_img_l(nh,"/left/image_rect",1000);  // 20HZ (50ms)
    message_filters::Subscriber<stereo_msgs::DisparityImage> sub_disp(nh,"/disparity",1000);
    typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, stereo_msgs::DisparityImage> ExactPolicy;
    message_filters::Synchronizer<ExactPolicy> sync(ExactPolicy(1000), sub_img_l, sub_disp);
    sync.registerCallback(boost::bind(&vision_callback, _1, _2));
    // ------------------------

    boost::thread th1(spin_thread);

    getchar();  // Plan A: read all msg, then begin to process

    get_stationary_imu();

    mtx.lock();

    process();

    mtx.unlock();

    fclose(solve_file);
    fclose(solve_omega_file);
    fclose(solve_vel_file);
    fclose(solve_acc_file);
    fclose(debug_imu_file);

    ros::shutdown();

    return 0;
}
