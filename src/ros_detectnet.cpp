#include <ros/ros.h>
#include <jetson-inference/detectNet.h>

#include <jetson-inference/loadImage.h>
#include <jetson-inference/cudaFont.h>

#include <jetson-inference/cudaMappedMemory.h>
#include <jetson-inference/cudaNormalize.h>

#include <opencv2/core.hpp>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <std_msgs/String.h>
#include <std_msgs/Int32.h>

#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

namespace ros_deep_learning{

class ros_detectnet : public nodelet::Nodelet
{
    public:
        ~ros_detectnet()
        {
            ROS_INFO("\nshutting down...\n");
            if(gpu_data)
                CUDA(cudaFree(gpu_data));
            delete net;
        }
        void onInit()
        {
            // get a private nodehandle
            ros::NodeHandle& private_nh = getPrivateNodeHandle();
            // get parameters from server, checking for errors as it goes
            
            std::string prototxt_path, model_path, mean_binary_path, class_labels_path;
            if(! private_nh.getParam("prototxt_path", prototxt_path) )
                ROS_ERROR("unable to read prototxt_path for detectnet node");
            if(! private_nh.getParam("model_path", model_path) )
                ROS_ERROR("unable to read model_path for detectnet node");

            // make sure files exist (and we can read them)
            if( access(prototxt_path.c_str(),R_OK) )
                 ROS_ERROR("unable to read file \"%s\", check filename and permissions",prototxt_path.c_str());
            if( access(model_path.c_str(),R_OK) )
                 ROS_ERROR("unable to read file \"%s\", check filename and permissions",model_path.c_str());

            // create imageNet
            //net = imageNet::Create(prototxt_path.c_str(),model_path.c_str(),NULL,class_labels_path.c_str());
            
            

            // create detectNet
            //net = detectNet::Create(detectNet::PEDNET, 0.5f, 2);
            net = detectNet::Create(prototxt_path.c_str(), model_path.c_str(), 0.0f, 0.5f, DETECTNET_DEFAULT_INPUT, DETECTNET_DEFAULT_COVERAGE, DETECTNET_DEFAULT_BBOX, 2);

            if( !net )
            {
                ROS_INFO("detectnet-console:   failed to initialize detectNet\n");
                return;
            }

            // setup image transport
            image_transport::ImageTransport it(private_nh);
            // subscriber for passing in images
            imsub = it.subscribe("imin", 10, &ros_detectnet::callback, this);
            
            impub = it.advertise("image_out", 1);
            
            // publisher for classifier output
            //class_pub = private_nh.advertise<std_msgs::Int32>("class",10);
            // publisher for human-readable classifier output
            //class_str_pub = private_nh.advertise<std_msgs::String>("class_str",10);

            // init gpu memory
            gpu_data = NULL;
        }


    private:
        void callback(const sensor_msgs::ImageConstPtr& input)
        {
            cv::Mat cv_im = cv_bridge::toCvCopy(input, "bgr8")->image;
            cv::Mat cv_result;
            
            //impub.publish(cv_bridge::CvImage(std_msgs::Header(), "bgr8", cv_im).toImageMsg());
            
            ROS_INFO("image ptr at %p",cv_im.data);
            // convert bit depth
            cv_im.convertTo(cv_im,CV_32FC3);
            // convert color
            cv::cvtColor(cv_im,cv_im,CV_BGR2RGBA);

            // allocate GPU data if necessary
            if(gpu_data == NULL){
                ROS_INFO("first allocation");
                CUDA(cudaMalloc(&gpu_data, cv_im.rows*cv_im.cols * sizeof(float4)));
            }else if(imgHeight != cv_im.rows || imgWidth != cv_im.cols){
                ROS_INFO("re allocation");
                // reallocate for a new image size if necessary
                CUDA(cudaFree(gpu_data));
                CUDA(cudaMalloc(&gpu_data, cv_im.rows*cv_im.cols * sizeof(float4)));
            }
            
            /*
            * allocate memory for output bounding boxes and class confidence
            */
            const uint32_t maxBoxes = net->GetMaxBoundingBoxes();		printf("maximum bounding boxes:  %u\n", maxBoxes);
            const uint32_t classes  = net->GetNumClasses();
            
            float* bbCPU    = NULL;
            float* bbCUDA   = NULL;
            float* confCPU  = NULL;
            float* confCUDA = NULL;
            
            if( !cudaAllocMapped((void**)&bbCPU, (void**)&bbCUDA, maxBoxes * sizeof(float4)) ||
                !cudaAllocMapped((void**)&confCPU, (void**)&confCUDA, maxBoxes * classes * sizeof(float)) )
            {
              printf("detectnet-console:  failed to alloc output memory\n");
            }

            int numBoundingBoxes = maxBoxes;
            
            imgHeight = cv_im.rows;
            imgWidth = cv_im.cols;
            imgSize = cv_im.rows*cv_im.cols * sizeof(float4);
            float4* cpu_data = (float4*)(cv_im.data);

            // copy to device
            CUDA(cudaMemcpy(gpu_data, cpu_data, imgSize, cudaMemcpyHostToDevice));

            bool det_result = net->Detect((float*)gpu_data, imgWidth, imgHeight, bbCPU, &numBoundingBoxes, confCPU);
            
            if(det_result)
            {
              int lastClass = 0;
              int lastStart = 0;
              
              for( int n=0; n < numBoundingBoxes; n++ )
              {
                const int nc = confCPU[n*2+1];
                float* bb = bbCPU + (n * 4);
                
                printf("bounding box %i   (%f, %f)  (%f, %f)  w=%f  h=%f\n", n, bb[0], bb[1], bb[2], bb[3], bb[2] - bb[0], bb[3] - bb[1]); 
                
                if( nc != lastClass || n == (numBoundingBoxes - 1) )
                {                  
                  
                  if( !net->DrawBoxes((float*)gpu_data, (float*)gpu_data, imgWidth, imgHeight, 
                                            bbCUDA + (lastStart * 4), (n - lastStart) + 1, lastClass) )
                    printf("detectnet-console:  failed to draw boxes\n");
                    
                  
                  //CUDA(cudaNormalizeRGBA((float4*)gpu_data, make_float2(0.0f, 255.0f), (float4*)gpu_data, make_float2(0.0f, 1.0f), imgWidth, imgHeight));
                  
                  // back to host
                  CUDA(cudaMemcpy(cpu_data, gpu_data, imgSize, cudaMemcpyDeviceToHost));
                  
                  lastClass = nc;
                  lastStart = n;
                  
                  /*
                  cv::namedWindow("Display window", cv::WINDOW_AUTOSIZE);
                  cv::imshow("Display window", cv::Mat(imgHeight, imgWidth, CV_8UC4, cpu_data));
                  cv::waitKey(0);
                  */

                  CUDA(cudaDeviceSynchronize());
                }
              }
            }
            else
            {
              std::cout << "detectnet: detection error occured" << std::endl;
            }
            
            //float confidence = 0.0f;

            // classify image
            //const int img_class = net->Classify((float*)gpu_data, imgWidth, imgHeight, &confidence);

            // publish class 
            /*
            std_msgs::Int32 class_msg;
            class_msg.data = img_class;
            class_pub.publish(class_msg);
            // publish class string
            std_msgs::String class_msg_str;
            class_msg_str.data = net->GetClassDesc(img_class);
            class_str_pub.publish(class_msg_str);

            if( img_class < 0 )
                ROS_INFO("imagenet-console:  failed to classify (result=%i)\n", img_class);

            */
            
            cv_result = cv::Mat(imgHeight, imgWidth, CV_32FC4, cpu_data);
            cv_result.convertTo(cv_result, CV_8UC4);
            
            cv::cvtColor(cv_result, cv_result, CV_RGBA2BGR);
                  
            impub.publish(cv_bridge::CvImage(std_msgs::Header(), "bgr8", cv_result).toImageMsg());
        }

        // private variables
        image_transport::Subscriber imsub;
        image_transport::Publisher impub;
        
        //ros::Publisher class_pub;
        //ros::Publisher class_str_pub;
        detectNet* net;

        float4* gpu_data;

        uint32_t imgWidth;
        uint32_t imgHeight;
        size_t   imgSize;
};

PLUGINLIB_DECLARE_CLASS(ros_deep_learning, ros_detectnet, ros_deep_learning::ros_detectnet, nodelet::Nodelet);

} // namespace ros_deep_learning
