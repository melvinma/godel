/*
	Copyright May 7, 2014 Southwest Research Institute

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

#include <godel_surface_detection/scan/robot_scan.h>
#include <godel_surface_detection/detection/surface_detection.h>
#include <godel_surface_detection/interactive/interactive_surface_server.h>
#include <godel_msgs/SurfaceDetection.h>
#include <godel_msgs/SelectSurface.h>
#include <godel_msgs/SelectedSurfacesChanged.h>
#include <godel_msgs/ProcessPlanning.h>
#include <godel_msgs/SurfaceBlendingParameters.h>
#include <godel_process_path_generation/VisualizeBlendingPlan.h>
#include <godel_process_path_generation/mesh_importer.h>
#include <godel_process_path_generation/utils.h>
#include <pcl/console/parse.h>


const std::string SURFACE_DETECTION_SERVICE = "surface_detection";
const std::string SURFACE_BLENDING_PARAMETERS_SERVICE = "surface_blending_parameters";
const std::string SELECT_SURFACE_SERVICE = "select_surface";
const std::string PROCESS_PATH_SERVICE="process_path";
const std::string VISUALIZE_BLENDING_PATH_SERVICE = "visualize_path_generator";
const std::string SELECTED_SURFACES_CHANGED_TOPIC = "selected_surfaces_changed";
const std::string ROBOT_SCAN_PATH_PREVIEW_TOPIC = "robot_scan_path_preview";
const std::string PUBLISH_REGION_POINT_CLOUD = "publish_region_point_cloud";
const std::string REGION_POINT_CLOUD_TOPIC="region_colored_cloud";

class SurfaceDetectionService
{
public:
	SurfaceDetectionService():
		publish_region_point_cloud_(false)
	{

	}

	~SurfaceDetectionService()
	{

	}

	bool init()
	{
		using namespace godel_surface_detection;

		ros::NodeHandle ph("~");

		// loading parameters
		ph.getParam(PUBLISH_REGION_POINT_CLOUD,publish_region_point_cloud_);

		// initializing surface detector
		if(surface_detection_.load_parameters("~/surface_detection") &&
				robot_scan_.load_parameters("~/robot_scan") &&
				load_blending_parameters("~/blending_plan",blending_plan_params_) &&
				surface_server_.load_parameters())
		{
			// save default parameters
			default_robot_scan_params__ = robot_scan_.params_;
			default_surf_detection_params_ = surface_detection_.params_;
			default_blending_plan_params_ = blending_plan_params_;


			ROS_INFO_STREAM("Surface detection service loaded parameters successfully");
			if(surface_detection_.init() && robot_scan_.init() && surface_server_.init())
			{
				// adding callbacks
				scan::RobotScan::ScanCallback cb = boost::bind(&detection::SurfaceDetection::add_cloud,&surface_detection_,_1);
				robot_scan_.add_scan_callback(cb);
				ROS_INFO_STREAM("Surface detection service initialization succeeded");
			}
			else
			{
				ROS_ERROR_STREAM("Surface detection service had an initialization error");
			}

		}
		else
		{
			ROS_ERROR_STREAM("Surface detection service failed to load parameters");
		}

		// start server
		interactive::InteractiveSurfaceServer::SelectionCallback f =	boost::bind(
				&SurfaceDetectionService::publish_selected_surfaces_changed,this);
		surface_server_.add_selection_callback(f);

		// initializing ros interface
		ros::NodeHandle nh;

		// service clients
		visualize_process_path_client_ =
				nh.serviceClient<godel_process_path_generation::VisualizeBlendingPlan>(VISUALIZE_BLENDING_PATH_SERVICE);

		// service servers
		surf_blend_parameters_server_ = nh.advertiseService(SURFACE_BLENDING_PARAMETERS_SERVICE,
				&SurfaceDetectionService::surface_blend_parameters_server_callback,this);

		surface_detect_server_ = nh.advertiseService(SURFACE_DETECTION_SERVICE,
				&SurfaceDetectionService::surface_detection_server_callback,this);

		select_surface_server_ = nh.advertiseService(SELECT_SURFACE_SERVICE,
				&SurfaceDetectionService::select_surface_server_callback,this);

		// publishers
		selected_surf_changed_pub_ = nh.advertise<godel_msgs::SelectedSurfacesChanged>(SELECTED_SURFACES_CHANGED_TOPIC,1);

		point_cloud_pub_ = nh.advertise<sensor_msgs::PointCloud2>(REGION_POINT_CLOUD_TOPIC,1);

		return true;
	}

	void run()
	{
		surface_server_.run();


		ros::Duration loop_duration(1.0f);
		while(ros::ok() && publish_region_point_cloud_)
		{
			if(!region_cloud_msg_.data.empty())
			{
				point_cloud_pub_.publish(region_cloud_msg_);
			}
			loop_duration.sleep();
		}


	}

protected:

	bool load_blending_parameters(std::string ns,godel_msgs::BlendingPlanParameters& params)
	{
		ros::NodeHandle nh(ns);
		return nh.getParam("tool_radius",params.tool_radius) &&
				nh.getParam("margin",params.margin) &&
				nh.getParam("overlap",params.overlap) &&
				nh.getParam("approach_spd",params.approach_spd) &&
				nh.getParam("blending_spd",params.blending_spd) &&
				nh.getParam("retract_spd",params.retract_spd) &&
				nh.getParam("traverse_spd",params.traverse_spd) &&
				nh.getParam("discretization",params.discretization) &&
				nh.getParam("safe_traverse_height",params.safe_traverse_height);
	}

	void publish_selected_surfaces_changed()
	{
		godel_msgs::SelectedSurfacesChanged msg;
		msg.selected_surfaces.clear();
		surface_server_.get_selected_list(msg.selected_surfaces);
		selected_surf_changed_pub_.publish(msg);
	}

	bool run_robot_scan(visualization_msgs::MarkerArray &surfaces)
	{
		bool succeeded = true;

		// publishing scan path preview
		robot_scan_.publish_scan_poses(ROBOT_SCAN_PATH_PREVIEW_TOPIC);

		// clear all results
		surface_detection_.clear_results();

		// saving parameters used

		ROS_INFO_STREAM("Starting scan");

		int scans_completed = robot_scan_.scan(false);
		if(scans_completed > 0)
		{
			ROS_INFO_STREAM("Scan points reached "<<scans_completed);
			succeeded = find_surfaces(surfaces);
		}
		else
		{
			succeeded = false;
			ROS_ERROR_STREAM("Scan failed");
		}
		return succeeded;
	}

	bool find_surfaces(visualization_msgs::MarkerArray &surfaces)
	{
		bool succeeded = true;
		if(surface_detection_.find_surfaces())
		{
			// clear current surfaces
			surface_server_.remove_all_surfaces();

			// adding meshes to server
			std::vector<pcl::PolygonMesh> meshes;
			surface_detection_.get_meshes(meshes);
			for(int i =0;i < meshes.size();i++)
			{
				surface_server_.add_surface(meshes[i]);
			}

			// copying to surface markers to output argument
			visualization_msgs::MarkerArray markers_msg = surface_detection_.get_surface_markers();
			surfaces.markers.insert(surfaces.markers.begin(),markers_msg.markers.begin(),markers_msg.markers.end());

			// saving latest successful results
			latest_surface_detection_results_.surface_detection = surface_detection_.params_;
			latest_surface_detection_results_.surfaces_found = true;
			latest_surface_detection_results_.surfaces = surfaces;
			robot_scan_.get_latest_scan_poses(latest_surface_detection_results_.robot_scan_poses);

			// saving region colored point cloud
			region_cloud_msg_ = sensor_msgs::PointCloud2();
			surface_detection_.get_region_colored_cloud(region_cloud_msg_);
		}
		else
		{
			succeeded = false;
			region_cloud_msg_ = sensor_msgs::PointCloud2();
		}

		return succeeded;
	}

	bool surface_detection_server_callback(godel_msgs::SurfaceDetection::Request &req,
			godel_msgs::SurfaceDetection::Response &res)
	{

		res.surfaces_found = false;
		res.surfaces = visualization_msgs::MarkerArray();

		switch(req.action)
		{
		case req.GET_CURRENT_PARAMETERS:
			res.robot_scan = robot_scan_.params_;
			res.surface_detection = surface_detection_.params_;
			break;

		case req.GET_DEFAULT_PARAMETERS:
			res.robot_scan = default_robot_scan_params__;
			res.surface_detection = default_surf_detection_params_;
			break;

		case req.PUBLISH_SCAN_PATH:

			if(req.use_default_parameters)
			{
				robot_scan_.params_ = default_robot_scan_params__;
				//surface_detection_.params_ = default_surf_detection_params_;
			}
			else
			{
				robot_scan_.params_ = req.robot_scan;
				//surface_detection_.params_ = req.surface_detection;
			}

			robot_scan_.publish_scan_poses(ROBOT_SCAN_PATH_PREVIEW_TOPIC);
			break;

		case req.SCAN_AND_FIND_ONLY:

			if(req.use_default_parameters)
			{
				robot_scan_.params_ = default_robot_scan_params__;
				surface_detection_.params_ = default_surf_detection_params_;
			}
			else
			{
				robot_scan_.params_ = req.robot_scan;
				surface_detection_.params_ = req.surface_detection;
			}

			res.surfaces_found =  run_robot_scan(res.surfaces);
			res.surfaces.markers.clear();
			break;

		case req.SCAN_FIND_AND_RETURN:

			if(req.use_default_parameters)
			{
				robot_scan_.params_ = default_robot_scan_params__;
				surface_detection_.params_ = default_surf_detection_params_;
			}
			else
			{
				robot_scan_.params_ = req.robot_scan;
				surface_detection_.params_ = req.surface_detection;
			}

			res.surfaces_found =  run_robot_scan(res.surfaces);
			break;

		case req.FIND_ONLY:

			if(req.use_default_parameters)
			{
				surface_detection_.params_ = default_surf_detection_params_;
			}
			else
			{
				surface_detection_.params_ = req.surface_detection;
			}

			res.surfaces_found =  find_surfaces(res.surfaces);
			res.surfaces.markers.clear();
			break;

		case req.FIND_AND_RETURN:

			if(req.use_default_parameters)
			{
				surface_detection_.params_ = default_surf_detection_params_;
			}
			else
			{
				surface_detection_.params_ = req.surface_detection;
			}

			res.surfaces_found =  find_surfaces(res.surfaces);
			break;

		case req.RETURN_LATEST_RESULTS:

			res = latest_surface_detection_results_;
			break;

		}

		return true;
	}

	bool select_surface_server_callback(godel_msgs::SelectSurface::Request &req, godel_msgs::SelectSurface::Response &res)
	{
		switch(req.action)
		{
		case req.SELECT:

			for(int i = 0; req.select_surfaces.size();i++)
			{
				surface_server_.set_selection_flag(req.select_surfaces[i],true);
			}
			break;

		case req.DESELECT:

			for(int i = 0; req.select_surfaces.size();i++)
			{
				surface_server_.set_selection_flag(req.select_surfaces[i],false);
			}
			break;

		case req.SELECT_ALL:

			surface_server_.select_all(true);
			break;

		case req.DESELECT_ALL:

			surface_server_.select_all(false);
			break;

		case req.HIDE_ALL:

			surface_server_.show_all(false);
			break;

		case req.SHOW_ALL:
			surface_server_.show_all(true);
			break;
		}

		return true;
	}

	bool process_path_server_callback(godel_msgs::ProcessPlanning::Request &req, godel_msgs::ProcessPlanning::Response &res)
	{
		ROS_WARN_STREAM("service call not implemented");
		res.succeeded = false;
		return true;
	}

	bool surface_blend_parameters_server_callback(godel_msgs::SurfaceBlendingParameters::Request &req,
			godel_msgs::SurfaceBlendingParameters::Response &res)
	{
		switch(req.action)
		{
		case req.GET_CURRENT_PARAMETERS:

			res.surface_detection = surface_detection_.params_;
			res.robot_scan = robot_scan_.params_;
			res.blending_plan = blending_plan_params_;
			break;

		case req.GET_DEFAULT_PARAMETERS:

			res.surface_detection = default_surf_detection_params_;
			res.robot_scan = default_robot_scan_params__;
			res.blending_plan = default_blending_plan_params_;
			break;
		}

		return true;
	}

protected:

	ros::ServiceServer surface_detect_server_;
	ros::ServiceServer select_surface_server_;
	ros::ServiceServer process_path_server_;
	ros::ServiceServer surf_blend_parameters_server_;
	ros::ServiceClient visualize_process_path_client_;
	ros::Publisher selected_surf_changed_pub_;

	// robot scan instance
	godel_surface_detection::scan::RobotScan robot_scan_;

	// surface detection instance
	godel_surface_detection::detection::SurfaceDetection surface_detection_;

	// marker server instance
	godel_surface_detection::interactive::InteractiveSurfaceServer surface_server_;

	// mesh importer for generating surface boundaries
	godel_process_path::MeshImporter mesh_importer_;

	// cloud publisher
	ros::Publisher point_cloud_pub_;

	// parameters
	godel_msgs::RobotScanParameters default_robot_scan_params__;
	godel_msgs::SurfaceDetectionParameters default_surf_detection_params_;
	godel_msgs::BlendingPlanParameters default_blending_plan_params_;
	godel_msgs::BlendingPlanParameters blending_plan_params_;
	godel_msgs::SurfaceDetection::Response latest_surface_detection_results_;

	// parameters
	bool publish_region_point_cloud_;

	// msgs
	sensor_msgs::PointCloud2 region_cloud_msg_;

};

int main(int argc,char** argv)
{
	ros::init(argc,argv,"surface_detection_server");
	ros::AsyncSpinner spinner(4);
	spinner.start();
	SurfaceDetectionService service;
	if(service.init())
	{
		service.run();
	}

	ros::waitForShutdown();
}

