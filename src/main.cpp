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
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/PoseStamped.h>
#include <sophus/se3.hpp>
#include <boost/thread.hpp>

#include "utils.h"

using namespace std;

queue<sensor_msgs::Imu> imu_buffer;
queue<geometry_msgs::PoseStamped> pose_buffer;
boost::mutex mtx;
Eigen::Matrix4d B;
vector<Sophus::SE3d> SE3_vec;
vector<double> pose_ts_vec;
vector<double> imu_ts_vec;
FILE *file;
FILE *debug_file;  // show the gt 10HZ pose
double deltaT = 0.1;  // 10HZ img (100ms)

void imu_callback(const sensor_msgs::ImuConstPtr& msg)
{
    mtx.lock();
    imu_buffer.push(*msg);
    mtx.unlock();
}

void pose_callback(const geometry_msgs::PoseStampedConstPtr& msg)
{
    mtx.lock();
    pose_buffer.push(*msg);
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
    while (!imu_buffer.empty()) imu_buffer.pop();
    while (!pose_buffer.empty()) pose_buffer.empty();
    SE3_vec.clear();
    pose_ts_vec.clear();
    imu_ts_vec.clear();
    file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/spline_pose.txt","w");
    debug_file = fopen("/home/timer/catkin_ws/src/cumulative_cubic_B_spline/helper/matlab_src/B_spline_plot/debug_gt_pose.txt","w");
    // --------
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
}

// ensure there is enough msg
void process()
{
    ros::Time start_time_stamp = pose_buffer.front().header.stamp;
    while (!pose_buffer.empty())
    {
        geometry_msgs::PoseStamped pose;
        pose = pose_buffer.front();
        pose_buffer.pop();
        pose_ts_vec.push_back((pose.header.stamp - start_time_stamp).toSec());

        Eigen::Quaterniond q;
        q.w() = pose.pose.orientation.w;
        q.x() = pose.pose.orientation.x;
        q.y() = pose.pose.orientation.y;
        q.z() = pose.pose.orientation.z;
        Eigen::Vector3d p;
        p(0) = pose.pose.position.x;
        p(1) = pose.pose.position.y;
        p(2) = pose.pose.position.z;
        Sophus::SE3d RT(q.toRotationMatrix(),p);
        SE3_vec.push_back(RT);

        Eigen::Matrix3d tmp_R = q.toRotationMatrix();
        Eigen::Vector3d theta = R_to_ypr(tmp_R);
        fprintf(debug_file,"%lf %lf %lf %lf %lf %lf %lf\n",
                            (pose.header.stamp - start_time_stamp).toSec(),
                            p(0),p(1),p(2),
                            theta(0),theta(1),theta(2)
               );
    }
    while (!imu_buffer.empty() && imu_buffer.front().header.stamp < start_time_stamp)
    {
        imu_buffer.pop();
    }
    while (!imu_buffer.empty())
    {
        sensor_msgs::Imu imu;
        imu = imu_buffer.front();
        imu_buffer.pop();
        imu_ts_vec.push_back((imu.header.stamp - start_time_stamp).toSec());
    }
    //---------------------
    double diff = 0.001;  // 1ms intepolate
    int l;
    l = SE3_vec.size();
    for (int i=1;i<l-2;i++)
    {
        Sophus::SE3d RTl0 = SE3_vec[i-1];
        for (double ts=pose_ts_vec[i];ts<pose_ts_vec[i+1];ts+=diff)
        {
            double u = (ts-pose_ts_vec[i])/deltaT;
            Eigen::Vector4d T1(1.0,u,u*u,u*u*u);
            Eigen::Vector4d T2;
            T2 = B*T1;

            vector<Sophus::SE3d> tmp_SE;
            tmp_SE.resize(4);
            for (int k=1;k<=3;k++)
            {
                int j = i-1+k;
                Eigen::VectorXd upsilon_omega = Sophus::SE3d::log(SE3_vec[j-1].inverse() * SE3_vec[j]);
                double B_select = T2(k); // B_j(u) -> B_k(u) ?  TUM's paper   k?   k-1?  :   k
                tmp_SE[k] = Sophus::SE3d::exp(B_select * upsilon_omega);
            }
            Sophus::SE3d res;
            res = tmp_SE[1] * tmp_SE[2] * tmp_SE[3];
            Sophus::SE3d ret = RTl0 * res;

            Eigen::Vector3d T,theta;
            Eigen::Matrix3d R;
            T = ret.translation();
            R = ret.rotationMatrix();
            theta = R_to_ypr(R);
            fprintf(file,"%lf %lf %lf %lf %lf %lf %lf\n",
                          ts,
                          T(0),T(1),T(2),                                           
                          theta(0),theta(1),theta(2)
                   );

/*
            // get omega
            vector<Sophus::SE3d> A,dA;
            A.resize(4);
            dA.resize(4);
            for (int j=1;j<=3;j++)  // 0 to 2 ? 1 to 3 ?  :  j= 1 to 3, diff with <Spline-fusion>
            {
                Eigen::VectorXd upsilon_omega = Sophus::SE3d::log(SE3_vec[i+j-2].inverse() * SE3_vec[i+j-1]);
                double B_select = T2(j);
                A[j] = Sophus::SE3d::exp(B_select * upsilon_omega);

                Eigen::Vector4d dT1(0.0,1.0,2*u,3*u*u);
                Eigen::Vector4d dT2;
                dT2 = 1.0/deltaT * B * dT1;
                double dB_select = dT2(j);
                dA[j] = A[j] * Sophus::SE3d::exp(upsilon_omega * dB_select);  // /omega * dB  -->  exp(/omega * dB) ?  Paper: Spline-Fusion
            }
            Sophus::SE3d dSE;
            Eigen::Matrix4d part1,part2,part3,all;  // ???
            part1 = (dA[1]*A[2]*A[3]).matrix();
            part2 = (A[1]*dA[2]*A[3]).matrix();
            part3 = (A[1]*A[2]*dA[3]).matrix();
            all = part1 + part2 + part3;
            Sophus::SE3d all_SE(all.block<3,3>(0,0),all.block<3,1>(0,3));
            dSE = RTl0 * all_SE;

            Eigen::Matrix3d spline_skew_omega = R.transpose() * dSE.rotationMatrix();
            cout << spline_skew_omega << endl;
            cout << "------------" << endl;
*/
        }
    }
}

int main(int argc, char **argv)
{
    ros::init(argc,argv,"Dense_Tracking");
    ros::NodeHandle nh("~");

    ros::Subscriber sub_imu = nh.subscribe("/imu0",1000,imu_callback);  // 200HZ (5ms)
    ros::Subscriber sub_pose = nh.subscribe("/self_calibration_estimator/pose",1000,pose_callback);  // 10HZ (100ms)

    init();

    boost::thread th1(spin_thread);

    getchar();
    mtx.lock();
    process();
    fclose(file);
    fclose(debug_file);
    mtx.unlock();
    ros::shutdown();

    return 0;
}