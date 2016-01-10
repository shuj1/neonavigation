#include <ros/ros.h>
#include <costmap/CSpace3D.h>
#include <nav_msgs/Path.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>
#include <sensor_msgs/PointCloud.h>

#include "grid_astar.hpp"


float signf(float a)
{
	if(a < 0) return -1;
	else if(a > 0) return 1;
	return 0;
}

namespace ros
{
	class NodeHandle_f: public NodeHandle
	{
	public:
		void param_cast(const std::string& param_name, 
				float& param_val, const float& default_val) const
		{
			double _default_val_d = (double)default_val;
			double _param_val_d;

			param<double>(param_name, _param_val_d, _default_val_d);
			param_val = (float)_param_val_d;
		}
		void param_cast(const std::string& param_name, 
				size_t& param_val, const size_t& default_val) const
		{
			int _default_val_d = (int)default_val;
			int _param_val_d;

			param<int>(param_name, _param_val_d, _default_val_d);
			param_val = (int)_param_val_d;
		}
		NodeHandle_f(const std::string& ns = std::string(), 
				const M_string& remappings = M_string())
		{
		}
	};
}

typedef grid_astar astar;
//typedef grid_astar<3, 2> astar;

class planner_3d
{
private:
	ros::NodeHandle_f nh;
	ros::Subscriber sub_map;
	ros::Subscriber sub_goal;
	ros::Publisher pub_path;
	ros::Publisher pub_debug;

	tf::TransformListener tfl;

	astar as;
	astar::gridmap<char, 0x40> cm;
	astar::gridmap<float> cost_estim_cache;
	
	astar::vecf euclid_cost_coef;
	float euclid_cost(const astar::vec &v, const astar::vecf coef)
	{
		auto vc = v;
		float cost = 0;
		for(int i = 0; i < noncyclic; i ++)
		{
			cost += powf(coef[i] * vc[i], 2.0);
		}
		cost = sqrtf(cost);
		for(int i = noncyclic; i < dim; i ++)
		{
			vc.cycle(vc[i], cm.size[i]);
			cost += fabs(coef[i] * vc[i]);
		}
		return cost;
	}
	float euclid_cost(const astar::vec &v)
	{
		return euclid_cost(v, euclid_cost_coef);
	}

	class rotation_cache
	{
	public:
		std::unique_ptr<astar::vecf[]> c;
		astar::vec size;
		int ser_size;
		void reset(const astar::vec &size)
		{
			size_t ser_size = 1;
			for(int i = 0; i < 3; i ++)
			{
				ser_size *= size[i];
			}
			this->size = size;
			this->ser_size = ser_size;

			c.reset(new astar::vecf[ser_size]);
		}
		rotation_cache(const astar::vec &size)
		{
			reset(size);
		}
		rotation_cache()
		{
		}
		astar::vecf &operator [](const astar::vec &pos)
		{
			size_t addr = pos[2];
			for(int i = 1; i >= 0; i --)
			{
				addr *= size[i];
				addr += pos[i];
			}
			return c[addr];
		}
	};
	std::vector<rotation_cache> rotgm;
	rotation_cache *rot_cache;

	costmap::MapMetaData3D map_info;
	std_msgs::Header map_header;
	float max_vel;
	float max_ang_vel;
	float freq;
	float search_range;
	int range;
	int unknown_cost;
	bool has_map;
	bool has_goal;
	std::vector<astar::vec> search_list;
	std::vector<astar::vec> search_list_rough;

	// Cost weights
	class cost_coeff_
	{
	public:
		float weight_decel;
		float weight_backward;
		float weight_ang_vel;
		float in_place_turn;
	} cc;

	geometry_msgs::PoseStamped goal;
	astar::vecf ec;
	astar::vecf ec_rough;

	void cb_goal(const geometry_msgs::PoseStamped::ConstPtr &msg)
	{
		goal = *msg;
		ROS_INFO("New goal received");
		has_goal = true;
		update_goal();
	}
	void update_goal()
	{	
		if(!has_map) return;
		astar::vec e;
		metric2grid(e[0], e[1], e[2],
				goal.pose.position.x, goal.pose.position.y, 
				tf::getYaw(goal.pose.orientation));

		astar::reservable_priority_queue<astar::pq> open;

		auto &g = cost_estim_cache;
		astar::vec p;
		for(p[1] = 0; p[1] < cm.size[1]; p[1] ++)
		{
			for(p[0] = 0; p[0] < cm.size[0]; p[0] ++)
			{
				p[2] = 0;
				g[p] = FLT_MAX;
			}
		}
		e[2] = 0;
		g[e] = -ec_rough[0] * 0.5; // Decrement to reduce calculation error
		open.push(astar::pq(g[e], g[e], e));
		while(true)
		{
			if(open.size() < 1) break;
			auto center = open.top();
			auto p = center.v;
			auto c = center.p_raw;
			open.pop();
			if(c > g[p]) continue;

			astar::vec d;
			d[2] = 0;

			for(d[0] = -range; d[0] <= range; d[0] ++)
			{
				for(d[1] = -range; d[1] <= range; d[1] ++)
				{
					if(hypotf(d[0], d[1]) > range) continue;
					if(d[0] == 0 && d[1] == 0) continue;
					auto next = p + d;
					if((unsigned int)next[0] >= (unsigned int)map_info.width ||
							(unsigned int)next[1] >= (unsigned int)map_info.height)
						continue;

					{
						float v[3], dp[3];
						float distf = d.len();
						int dist = distf;
						distf /= dist;
						v[0] = p[0];
						v[1] = p[1];
						v[2] = 0;
						dp[0] = (float)d[0] / dist;
						dp[1] = (float)d[1] / dist;
						astar::vec pos(v);
						char c = 0;
						for(int i = 0; i < dist; i ++)
						{
							pos[0] = lroundf(v[0]);
							pos[1] = lroundf(v[1]);
							c = cm[pos];
							if(c > 99) break;
							v[0] += dp[0];
							v[1] += dp[1];
						}
						if(c > 99) continue;
					}

					float cost = euclid_cost(d, ec_rough);
					auto gp = c + cost;
					if(g[next] > gp)
					{
						g[next] = gp;
						open.push(astar::pq(gp, gp, next));
					}
				}
			}
		}
		g[e] = 0;
		sensor_msgs::PointCloud debug;
		debug.header = map_header;
		debug.header.stamp = ros::Time::now();
		{
			astar::vec p;
			for(p[1] = 0; p[1] < cm.size[1]; p[1] ++)
			{
				for(p[0] = 0; p[0] < cm.size[0]; p[0] ++)
				{
					p[2] = 0;
					if(cost_estim_cache[p] == FLT_MAX) continue;
					float x, y, yaw;
					grid2metric(p[0], p[1], p[2], x, y, yaw);
					geometry_msgs::Point32 point;
					point.x = x;
					point.y = y;
					point.z = cost_estim_cache[p] / 500;
					debug.points.push_back(point);
				}
			}
		}
		pub_debug.publish(debug);
		ROS_INFO("Cost estimation cache generated");
	}
	void cb_map(const costmap::CSpace3D::ConstPtr &msg)
	{
		ROS_INFO("Map received");
		ROS_INFO(" linear_resolution %0.2f x (%dx%d) px", msg->info.linear_resolution,
				msg->info.width, msg->info.height);
		ROS_INFO(" angular_resolution %0.2f x %d px", msg->info.angular_resolution,
				msg->info.angle);
		ROS_INFO(" origin %0.3f m, %0.3f m, %0.3f rad", 
				msg->info.origin.position.x,
				msg->info.origin.position.y,
				tf::getYaw(msg->info.origin.orientation));

		float ec_val[3] = {1.0f / max_vel, 
			1.0f / max_vel, 
			1.0f * cc.weight_ang_vel / max_ang_vel};

		ec = astar::vecf(ec_val);
		ec_val[2] = 0;
		ec_rough = astar::vecf(ec_val);

		if(map_info.linear_resolution != msg->info.linear_resolution ||
				map_info.angular_resolution != msg->info.angular_resolution)
		{
			astar::vec d;
			range = (int)(search_range / msg->info.linear_resolution);

			search_list.clear();
			for(d[0] = -range; d[0] <= range; d[0] ++)
			{
				for(d[1] = -range; d[1] <= range; d[1] ++)
				{
					if(hypotf(d[0], d[1]) > range) continue;
					for(d[2] = 0; d[2] < (int)msg->info.angle; d[2] ++)
					{
						search_list.push_back(d);
					}
				}
			}
			search_list_rough.clear();
			for(d[0] = -range; d[0] <= range; d[0] ++)
			{
				for(d[1] = -range; d[1] <= range; d[1] ++)
				{
					if(hypotf(d[0], d[1]) > range) continue;
					if(hypotf(d[0], d[1]) < range/2) continue;
					d[2] = 0;
					search_list_rough.push_back(d);
				}
			}
			ROS_INFO("Search list updated (range: ang %d, lin %d) %d", 
					msg->info.angle, range, (int)search_list.size());

			rotgm.resize(msg->info.angle);
			for(int i = 0; i < (int)msg->info.angle; i ++)
			{
				int size[3] = {range * 2 + 1, range * 2 + 1, (int)msg->info.angle};
				auto &r = rotgm[i];
				r.reset(astar::vec(size));

				astar::vec d;

				for(d[0] = 0; d[0] <= range * 2; d[0] ++)
				{
					for(d[1] = 0; d[1] <= range * 2; d[1] ++)
					{
						for(d[2] = 0; d[2] < (int)msg->info.angle; d[2] ++)
						{
							float val[3] = {
								(d[0] - range) * msg->info.linear_resolution, 
								(d[1] - range) * msg->info.linear_resolution, 
								d[2] * msg->info.angular_resolution};
							auto v = astar::vecf(val);
							rotate(v, -i * msg->info.angular_resolution);
							r[d] = v;
						}
					}
				}
			}
			ROS_INFO("Rotation cache generated");
		}
		map_info = msg->info;
		map_header = msg->header;

		int size[3] = {(int)map_info.width, (int)map_info.height, (int)map_info.angle};
		as.reset(astar::vec(size));
		cm.reset(astar::vec(size));
		size[2] = 1;
		cost_estim_cache.reset(astar::vec(size));

		astar::vec p;
		for(p[2] = 0; p[2] < size[2]; p[2] ++)
		{
			for(p[1] = 0; p[1] < size[1]; p[1] ++)
			{
				for(p[0] = 0; p[0] < size[0]; p[0] ++)
				{
					size_t addr = ((p[2] * size[1]) + p[1]) * size[0] + p[0];
					cm[p] = msg->data[addr];
					if(cm[p] < 0) cm[p] = unknown_cost;
				}
			}
		}
		ROS_INFO("Map copied");

		has_map = true;
		update_goal();
	}

public:
	planner_3d():
		nh("~")
	{
		sub_map = nh.subscribe("costmap", 1, &planner_3d::cb_map, this);
		sub_goal = nh.subscribe("goal", 1, &planner_3d::cb_goal, this);
		pub_path = nh.advertise<nav_msgs::Path>("path", 1, true);
		pub_debug = nh.advertise<sensor_msgs::PointCloud>("debug", 1, true);

		nh.param_cast("freq", freq, 0.5f);
		nh.param_cast("search_range", search_range, 0.4f);

		nh.param_cast("max_vel", max_vel, 0.3f);
		nh.param_cast("max_ang_vel", max_ang_vel, 0.6f);

		nh.param_cast("weight_decel", cc.weight_decel, 50.0f);
		nh.param_cast("weight_backward", cc.weight_backward, 100.0f);
		nh.param_cast("weight_ang_vel", cc.weight_ang_vel, 0.3f);
		nh.param_cast("cost_in_place_turn", cc.in_place_turn, 50.0f);
		
		nh.param("unknown_cost", unknown_cost, 100);

		int queue_size_limit;
		nh.param("queue_size_limit", queue_size_limit, 0);
		as.set_queue_size_limit(queue_size_limit);

		has_map = false;
		has_goal = false;
	}
	void spin()
	{
		ros::Rate wait(freq);
		ROS_INFO("Initialized");

		while(ros::ok())
		{
			wait.sleep();
			ros::spinOnce();
			if(has_map && has_goal)
			{
				geometry_msgs::PoseStamped start;
				start.header.frame_id = "base_link";
				start.header.stamp = ros::Time::now();
				start.pose.orientation = tf::createQuaternionMsgFromYaw(0.0);
				try
				{
					tfl.waitForTransform("base_link", map_header.frame_id, 
							map_header.stamp, ros::Duration(0.1));
					tfl.transformPose(map_header.frame_id, start, start);
				}
				catch(tf::TransformException &e)
				{
					ROS_INFO("Transform failed");
					continue;
				}

				nav_msgs::Path path;
				make_plan(start.pose, goal.pose, path);
				pub_path.publish(path);
			}
		}
	}

private:
	void grid2metric(
			const int x, const int y, const int yaw,
			float &gx, float &gy, float &gyaw)
	{
		gx = x * map_info.linear_resolution + map_info.origin.position.x;
		gy = y * map_info.linear_resolution + map_info.origin.position.y;
		gyaw = yaw * map_info.angular_resolution;
	}
	void metric2grid(
			int &x, int &y, int &yaw,
			const float gx, const float gy, const float gyaw)
	{
		x = lroundf((gx - map_info.origin.position.x) / map_info.linear_resolution);
		y = lroundf((gy - map_info.origin.position.y) / map_info.linear_resolution);
		yaw = lroundf(gyaw / map_info.angular_resolution);
	}
	bool make_plan(const geometry_msgs::Pose &gs, const geometry_msgs::Pose &ge, 
			nav_msgs::Path &path)
	{
		astar::vec s, e;
		metric2grid(s[0], s[1], s[2],
				gs.position.x, gs.position.y, tf::getYaw(gs.orientation));
		metric2grid(e[0], e[1], e[2],
				ge.position.x, ge.position.y, tf::getYaw(ge.orientation));

		ROS_INFO("Planning from (%d, %d, %d) to (%d, %d, %d)",
				s[0], s[1], s[2], e[0], e[1], e[2]);
		std::list<astar::vec> path_grid;
		auto ts = std::chrono::high_resolution_clock::now();
		if(!as.search(s, e, path_grid, 
				std::bind(&planner_3d::cb_cost, 
					this, std::placeholders::_1, std::placeholders::_2), 
				std::bind(&planner_3d::cb_cost_estim, 
					this, std::placeholders::_1, std::placeholders::_2), 
				std::bind(&planner_3d::cb_search, 
					this, std::placeholders::_1,
					std::placeholders::_2, std::placeholders::_3), 
				std::bind(&planner_3d::cb_progress, 
					this, std::placeholders::_1), 
				1.0f / freq))
		{
			ROS_INFO("Search failed");
			return false;
		}
		auto tnow = std::chrono::high_resolution_clock::now();
		printf("time: %0.3f\n", std::chrono::duration<float>(tnow - ts).count());

		path.header = map_header;
		path.header.stamp = ros::Time::now();
		for(auto &p: path_grid)
		{
			float x, y, yaw;
			grid2metric(p[0], p[1], p[2], x, y, yaw);
			geometry_msgs::PoseStamped ps;
			ps.header = path.header;
			ps.pose.position.x = x;
			ps.pose.position.y = y;
			ps.pose.position.z = 0;
			ps.pose.orientation =
				tf::createQuaternionMsgFromYaw(yaw);
			path.poses.push_back(ps);
		}

		return true;
	}
	bool rough;
	astar::vec v_goal;
	std::vector<astar::vec> &cb_search(
			const astar::vec& p,
			const astar::vec& s, const astar::vec& e)
	{
		v_goal = e;
		auto ds = s - p;
		rot_cache = &rotgm[p[2]];
		
		if(ds.sqlen() < 16*16)
		{
			rough = false;
			euclid_cost_coef = ec;
			return search_list;
		}
		rough = true;
		euclid_cost_coef = ec_rough;
		return search_list_rough;
	}
	bool cb_progress(const std::list<astar::vec>& path_grid)
	{
		ROS_INFO("Search timed out");
		return true;
	}
	void rotate(astar::vecf &v, const float ang)
	{
		astar::vecf tmp = v;
		float cos_v = cosf(ang);
		float sin_v = sinf(ang);

		v[0] = cos_v * tmp[0] - sin_v * tmp[1];
		v[1] = sin_v * tmp[0] + cos_v * tmp[1];
		v[2] = v[2] + ang;
		if(v[2] > M_PI) v[2] -= 2 * M_PI;
		else if(v[2] < -M_PI) v[2] += 2 * M_PI;
	}
	float cb_cost_estim(const astar::vec &s, const astar::vec &e)
	{
		auto s2 = s;
		s2[2] = 0;
		return cost_estim_cache[s2];
	}
	float cb_cost(const astar::vec &s, astar::vec &e)
	{
		auto d = e - s;
		float cost = euclid_cost(d);

		if(rough)
		{
			if((e - v_goal).len() < range / 2) e = v_goal;
			// Go-straight
			float v[3], dp[3], sum = 0;
			float distf = d.len();
			int dist = distf;
			distf /= dist;
			v[0] = s[0];
			v[1] = s[1];
			v[2] = 0;
			dp[0] = (float)d[0] / dist;
			dp[1] = (float)d[1] / dist;
			astar::vec pos(v);
			for(int i = 0; i < dist; i ++)
			{
				pos[0] = lroundf(v[0]);
				pos[1] = lroundf(v[1]);
				auto c = cm[pos];
				if(c > 99) return -1;
				sum += c;
				v[0] += dp[0];
				v[1] += dp[1];
			}
			if(e[0] == v_goal[0] && e[1] == v_goal[1])
			{
				e[2] = v_goal[2];
			}
			else
			{
				e[2] = lroundf(atan2f(d[1], d[0]) / map_info.angular_resolution);
				if(e[2] < 0) e[2] += map_info.angle;
			}
			cost += sum * map_info.linear_resolution * distf;
			return cost;
		}
		if(d[0] == 0 && d[1] == 0)
		{
			// In-place turn
			return cc.in_place_turn;
		}

		/*float diff_val[3] = {
			d[0] * map_info.linear_resolution, 
			d[1] * map_info.linear_resolution, 
			e[2] * map_info.angular_resolution};
		astar::vecf motion(diff_val);
		rotate(motion, -s[2] * map_info.angular_resolution);*/
		astar::vec d2;
		d2[0] = d[0] + range;
		d2[1] = d[1] + range;
		d2[2] = e[2];
		astar::vecf motion = (*rot_cache)[d2];
		
		astar::vecf motion_grid = motion;
		motion_grid[0] /= map_info.linear_resolution;
		motion_grid[1] /= map_info.linear_resolution;
		motion_grid[2] /= map_info.angular_resolution;

		if(lroundf(motion_grid[0]) == 0 && lroundf(motion_grid[1]) != 0)
		{
			// Not non-holonomic
			return -1;
		}
		if(lroundf(motion_grid[2]) == 0 && lroundf(motion_grid[1]) != 0)
		{
			// Drifted
			return -1;
		}

		if(fabs(motion[2]) >= 2.0 * M_PI / 4.0)
		{
			// Over 90 degree turn
			// must be separated into two curves
			return -1;
		}

		float dist = motion.len();

		float cos_v = cosf(motion[2]);
		float sin_v = sinf(motion[2]);

		bool forward(true);
		if(motion[0] < 0) forward = false;

		if(!forward)
		{
			cost += cc.weight_backward * dist;
		}

		if(lroundf(motion_grid[2]) == 0)
		{
			// Go-straight
			float v[3], dp[3], sum = 0;
			float distf = d.len();
			int dist = distf;
			distf /= dist;
			v[0] = s[0];
			v[1] = s[1];
			v[2] = 0;
			dp[0] = (float)d[0] / dist;
			dp[1] = (float)d[1] / dist;
			astar::vec pos(v);
			for(int i = 0; i < dist; i ++)
			{
				pos[0] = lroundf(v[0]);
				pos[1] = lroundf(v[1]);
				auto c = cm[pos];
				if(c > 99) return -1;
				sum += c;
				v[0] += dp[0];
				v[1] += dp[1];
			}
			cost += sum * map_info.linear_resolution * distf;
		}
		else
		{
			// Curve
			if(motion[0] * motion[1] * motion[2] < 0)
			{
				return -1;
			}

			float r1 = motion[1] + motion[0] * cos_v / sin_v;
			float r2 = sqrtf(powf(motion[0], 2.0) + powf(motion[0] * cos_v / sin_v, 2.0));
			if((sinf(motion[2]) < 0) ^ (!forward)) r2 = -r2;

			// curveture at the start pose and the end pose must be same
			if(fabs(r1 - r2) > map_info.linear_resolution * sqrtf(2.0))
			{
				// Drifted
				return -1;
			}

			float curv_radius = r1;

			float vel = max_vel;
			float ang_vel = cos_v * vel / (cos_v * motion[0] + sin_v * motion[1]);
			if(fabs(ang_vel) > max_ang_vel)
			{
				ang_vel = signf(ang_vel) * max_ang_vel;
				vel = fabs(curv_radius) * max_ang_vel;

				// Curve deceleration penalty
				cost += dist * fabs(vel / max_vel) * cc.weight_decel;
			}

			{
				// Go-straight
				float v[3], dp[3], sum = 0;
				float distf = d.len();
				int dist = distf;
				distf /= dist;
				v[0] = s[0];
				v[1] = s[1];
				v[2] = 0;
				dp[0] = (float)d[0] / dist;
				dp[1] = (float)d[1] / dist;
				astar::vec pos(v);
				for(int i = 0; i < dist; i ++)
				{
					pos[0] = lroundf(v[0]);
					pos[1] = lroundf(v[1]);
					auto c = cm[pos];
					if(c > 99) return -1;
					sum += c;
					v[0] += dp[0];
					v[1] += dp[1];
				}
				cost += sum * map_info.linear_resolution * distf;
			}
		}

		return cost;
	}
};

int main(int argc, char *argv[])
{
	ros::init(argc, argv, "planner_3d");
	
	planner_3d jy;
	jy.spin();

	return 0;
}

