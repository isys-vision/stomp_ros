#include "openvdb_distance_field.h"
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/GridOperators.h>
#include <openvdb/tools/VolumeToSpheres.h>
#include <openvdb/tools/GridTransformer.h>
#include <openvdb/tools/Interpolation.h>
#include <ros/assert.h>
#include <algorithm>
#include <math.h>

const static std::string voxel_size_meta_name = "voxel_size";
const static std::string background_meta_name = "background";
const static std::string ex_bandwidth_meta_name = "exBandWidth";
const static std::string in_bandwidth_meta_name = "inBandWidth";

distance_field::CollisionRobotOpenVDB::CollisionRobotOpenVDB(const moveit::core::RobotModelConstPtr &model,
                                                             const float voxel_size, const float background,
                                                             const float exBandWidth, const float inBandWidth)
  : robot_model_(model), voxel_size_(voxel_size), background_(background),
    exBandWidth_(exBandWidth), inBandWidth_(inBandWidth),
    links_(model->getLinkModelsWithCollisionGeometry())
{
  createDefaultAllowedCollisionMatrix();
  createStaticSDFs();
  createActiveSDFs();
  createDynamicSDFs();
  createDefaultDistanceQuery();
}

distance_field::CollisionRobotOpenVDB::CollisionRobotOpenVDB(const moveit::core::RobotModelConstPtr &model,
                                                             const std::string &file_path)
  : robot_model_(model), links_(model->getLinkModelsWithCollisionGeometry())
{
  // Step 1: Load the OpenVDB archive
  auto grid_data = readFromFile(file_path);
  const auto& grids = *grid_data.first;
  const auto& metadata = *grid_data.second;

  // Step 2: Process meta-data at the file level - load nominal voxel size, background, etc...
  voxel_size_ = metadata.metaValue<float>(voxel_size_meta_name);
  background_ = metadata.metaValue<float>(background_meta_name);
  exBandWidth_ = metadata.metaValue<float>(ex_bandwidth_meta_name);
  inBandWidth_ = metadata.metaValue<float>(in_bandwidth_meta_name);


  // Step 3: For each static, active, & dynamic link, let's construct it's distance field
  //         using archived data.


  // Step 4: Perform sanity checking (e.g. do all links have an associated distance field?)
  throw std::runtime_error("Not implemented");
}

void distance_field::CollisionRobotOpenVDB::createStaticSDFs()
{
  const robot_model::LinkModel *root_link = robot_model_->getRootLink();

  // Check to make sure link has collision geometry to add. I don't think this is required,
  // because it will be world link and I don't think it will ever have geometry.
  if (std::find(links_.begin(), links_.end(), robot_model_->getRootLink()) != links_.end())
  {
    OpenVDBDistanceFieldPtr sdf(new OpenVDBDistanceField(voxel_size_, background_));
    sdf->addLinkToField(root_link, Eigen::Affine3d::Identity(), exBandWidth_, inBandWidth_);
    static_links_.push_back(root_link);
    static_sdf_.push_back(OpenVDBDistanceFieldConstPtr(sdf));
  }

  std::vector<const robot_model::LinkModel*> models;
  addAssociatedFixedTransforms(root_link, models);
}

void distance_field::CollisionRobotOpenVDB::createActiveSDFs()
{
  const std::vector<const robot_model::JointModelGroup*> groups = robot_model_->getJointModelGroups();
  for (std::size_t i = 0 ; i < groups.size() ; ++i)
  {
    const std::vector<const robot_model::LinkModel*> links = groups[i]->getLinkModels();
    for (std::size_t j = 0 ; j < links.size() ; ++j)
    {
      auto it = std::find(active_links_.begin(), active_links_.end(), links[j]);
      auto it2 = std::find(links_.begin(), links_.end(), links[j]); // Check to make sure it has collision geometry
      if (it == active_links_.end() && it2 != links_.end())
      {
        active_links_.push_back(links[j]);
      }
    }
  }

  active_sdf_.resize(active_links_.size());
  active_spheres_.resize(active_links_.size());
  for (std::size_t i = 0 ; i < active_links_.size() ; ++i)
  {
    OpenVDBDistanceFieldPtr sdf;
    float v = voxel_size_;

    // This dynamicly changes the voxel size to try and ensure that a sphere model is found.
    for (std::size_t j = 0 ; j < 10 ; ++j)
    {
      sdf.reset(new OpenVDBDistanceField(v, background_));

      sdf->addLinkToField(active_links_[i], Eigen::Affine3d::Identity(), (voxel_size_/v) * exBandWidth_, (voxel_size_/v) * inBandWidth_);

      const auto n_spheres = 20;
      const auto can_overlap = true;
      const auto min_voxel_size = 1.0f;
      const auto max_voxel_size = std::numeric_limits<float>::max();
      const auto iso_surface = 0.0f; // the value at which the surface exists; 0.0 for solid models (clouds are different)
      const auto n_instances = 100000; // num of voxels to consider when fitting spheres

      sdf->fillWithSpheres(active_spheres_[i], n_spheres, can_overlap, min_voxel_size,
                           max_voxel_size, iso_surface, n_instances);

      if (active_spheres_[i].size() > 1) // openvdb appears to ALWAYS insert one sphere, so we want more
        break;

      v = v * 0.5; // try again with voxels of half the size
    }

    if (active_spheres_[i].size() == 0)
    {
      ROS_ERROR("Unable to generate spheres for link: %s", active_links_[i]->getName().c_str());
    }

    active_sdf_[i] = OpenVDBDistanceFieldConstPtr(sdf);
  }
}

void distance_field::CollisionRobotOpenVDB::createDynamicSDFs()
{
  dynamic_links_ = links_;

  // remove static links from list
  for (std::size_t i = 0 ; i < static_links_.size() ; ++i)
  {
    dynamic_links_.erase(std::remove(dynamic_links_.begin(), dynamic_links_.end(), static_links_[i]));
  }

  // remove active links from list
  for (std::size_t i = 0 ; i < active_links_.size() ; ++i)
  {
    dynamic_links_.erase(std::remove(dynamic_links_.begin(), dynamic_links_.end(), active_links_[i]));
  }

  dynamic_sdf_.resize(dynamic_links_.size());
  for (std::size_t i = 0 ; i < dynamic_links_.size() ; ++i)
  {
    OpenVDBDistanceFieldPtr sdf(new OpenVDBDistanceField(voxel_size_, background_));

    sdf->addLinkToField(dynamic_links_[i], Eigen::Affine3d::Identity(), exBandWidth_, inBandWidth_);

    dynamic_sdf_[i] = OpenVDBDistanceFieldConstPtr(sdf);
  }
}

void distance_field::CollisionRobotOpenVDB::addAssociatedFixedTransforms(const robot_model::LinkModel *link,
                                                                         std::vector<const robot_model::LinkModel*>& links_so_far)
{
  const moveit::core::LinkTransformMap fixed_attached = link->getAssociatedFixedTransforms();

  for (auto it = fixed_attached.begin(); it!=fixed_attached.end(); ++it)
  {
    // only add child links
    if (std::find(links_so_far.begin(), links_so_far.end(), it->first) == links_so_far.end())
    {
      links_so_far.push_back(it->first);
      // Check to make sure link has collision geometry to add
      if (std::find(links_.begin(), links_.end(), it->first) != links_.end())
      {
        OpenVDBDistanceFieldPtr sdf(new OpenVDBDistanceField(voxel_size_, background_));
        sdf->addLinkToField(it->first, it->second, exBandWidth_, inBandWidth_);
        static_links_.push_back(it->first);
        static_sdf_.push_back(OpenVDBDistanceFieldConstPtr(sdf));
      }

      addAssociatedFixedTransforms(it->first, links_so_far);
    }
  }
}

void distance_field::CollisionRobotOpenVDB::writeToFile(const std::string& file_path)
{
  // Create a VDB file object.
  openvdb::io::File vdbFile(file_path);

  // Add the static grids to grid array
  openvdb::GridPtrVec grids;

  for (std::size_t i = 0 ; i < static_sdf_.size() ; ++i)
  {
    grids.push_back(static_sdf_[i]->getGrid());
  }

  // Add the dynamic grids to grid array
  for (std::size_t i = 0 ; i < dynamic_sdf_.size() ; ++i)
  {
    grids.push_back(dynamic_sdf_[i]->getGrid());
  }

  for (std::size_t i = 0 ; i < active_sdf_.size() ; ++i)
  {
    grids.push_back(active_sdf_[i]->getGrid());
  }

  vdbFile.write(grids);
  vdbFile.close();
}

std::pair<openvdb::GridPtrVecPtr, openvdb::MetaMap::Ptr>
distance_field::CollisionRobotOpenVDB::readFromFile(const std::string& file_path)
{
  openvdb::io::File file(file_path);
  // Open the file.  This reads the file header, but not any grids.
  file.open();

  if (!file.isOpen())
  {
    const std::string error_text = "Unable to load openvdb models from file: " + file_path;
    throw std::runtime_error(error_text);
  }

  std::pair<openvdb::GridPtrVecPtr, openvdb::MetaMap::Ptr> grid_data;
  grid_data.first = file.getGrids();
  grid_data.second = file.getMetadata();

  // Error checking
  if (!grid_data.first) throw std::runtime_error("Unable to load any grids from file " + file_path);
  if (!grid_data.second) throw std::runtime_error("Unable to load meta-data from file: " + file_path);

  file.close(); // RAII?

  return grid_data;
}


static openvdb::math::Transform::Ptr makeTransform(const openvdb::FloatGrid& grid, const openvdb::Mat4d& tf)
{
  auto ptr = openvdb::math::Transform::createLinearTransform(tf);
  ptr->preScale(grid.transformPtr()->voxelSize());
  return ptr;
}


std::pair<distance_field::PointCloud::Ptr, distance_field::PointCloud::Ptr>
distance_field::CollisionRobotOpenVDB::voxelGridToPointClouds(const moveit::core::RobotState &state) const
{
  return voxelGridToPointClouds(state, std::vector<std::string>());
}

std::pair<distance_field::PointCloud::Ptr, distance_field::PointCloud::Ptr>
distance_field::CollisionRobotOpenVDB::voxelGridToPointClouds(const moveit::core::RobotState &state,
                                                              const std::vector<std::string>& exclude_list) const
{
  std::pair<distance_field::PointCloud::Ptr, distance_field::PointCloud::Ptr> pair;
  pair.first.reset(new distance_field::PointCloud());
  pair.second.reset(new distance_field::PointCloud());

  auto& inside_cloud = *pair.first;
  auto& outside_cloud = *pair.second;

  // Active links
  for (std::size_t i = 0; i < active_links_.size(); ++i)
  {
    if (std::find(exclude_list.begin(), exclude_list.end(),
                  active_links_[i]->getName()) != exclude_list.end()) continue;

    openvdb::Mat4d tf;
    Affine3dToMat4dAffine(state.getGlobalLinkTransform(active_links_[i]), tf);
    auto transform = makeTransform(*active_sdf_[i]->getGrid(), tf);

    auto copy = active_sdf_[i]->getGrid()->deepCopy();
    copy->setTransform(transform);
    auto f = distance_field::toInsideOutsidePointCloud(*copy);

    inside_cloud.insert(inside_cloud.end(), f.first->begin(), f.first->end());
    outside_cloud.insert(outside_cloud.end(), f.second->begin(), f.second->end());
  }

  // Dynamic links
  for (std::size_t i = 0; i < dynamic_links_.size(); ++i)
  {
    if (std::find(exclude_list.begin(), exclude_list.end(), dynamic_links_[i]->getName()) != exclude_list.end()) continue;

    openvdb::Mat4d tf;
    Affine3dToMat4dAffine(state.getGlobalLinkTransform(dynamic_links_[i]), tf);
    auto transform = makeTransform(*dynamic_sdf_[i]->getGrid(), tf);

    auto copy = dynamic_sdf_[i]->getGrid()->deepCopy();
    copy->setTransform(transform);
    auto f = distance_field::toInsideOutsidePointCloud(*copy);

    inside_cloud.insert(inside_cloud.end(), f.first->begin(), f.first->end());
    outside_cloud.insert(outside_cloud.end(), f.second->begin(), f.second->end());
  }

  // static links
  for (std::size_t i = 0; i < static_links_.size(); ++i)
  {
    if (std::find(exclude_list.begin(), exclude_list.end(),
                  static_links_[i]->getName()) != exclude_list.end()) continue;

    auto grid = static_sdf_[i]->getGrid();
    auto f = distance_field::toInsideOutsidePointCloud(*grid);

    inside_cloud.insert(inside_cloud.end(), f.first->begin(), f.first->end());
    outside_cloud.insert(outside_cloud.end(), f.second->begin(), f.second->end());
  }

  return pair;
}

visualization_msgs::MarkerArray
distance_field::CollisionRobotOpenVDB::spheresToVisualizationMarkers(const moveit::core::RobotState &state) const
{
  std::vector<DistanceQueryData> dist_query(dist_query_);

  visualization_msgs::MarkerArray ma;
  int marker_id = 0;

  for (std::size_t i = 0; i < active_links_.size(); ++i)
  {
    openvdb::Mat4d tf;
    Affine3dToMat4d(state.getGlobalLinkTransform(active_links_[i]), tf);
    dist_query[i].spheres = active_spheres_[i];

    // transform sphere origins into world coordinate system
    std::transform(dist_query[i].spheres.begin(), dist_query[i].spheres.end(), dist_query[i].spheres.begin(),
                   [&tf](std::pair<openvdb::math::Vec3d, double> p){ p.first = (tf * p.first); return p; });

    for (const auto& sphere : dist_query[i].spheres)
    {
      auto origin = sphere.first;
      auto radius = sphere.second;
      auto combined = openvdb::math::Vec4s(origin.x(), origin.y(), origin.z(), radius);

      auto m = distance_field::toSphere(combined, marker_id++);
      ma.markers.push_back(m);
    }
  }

  return ma;
}

uint64_t distance_field::CollisionRobotOpenVDB::memUsage() const
{
  uint64_t mem_usage = 0;

  for (std::size_t i = 0 ; i < static_sdf_.size() ; ++i)
  {
    mem_usage += static_sdf_[i]->memUsage();
  }

  for (std::size_t i = 0 ; i < dynamic_sdf_.size() ; ++i)
  {
    mem_usage += dynamic_sdf_[i]->memUsage();
  }

  for (std::size_t i = 0 ; i < active_sdf_.size() ; ++i)
  {
    mem_usage += active_sdf_[i]->memUsage();
  }

  return mem_usage;
}

void distance_field::CollisionRobotOpenVDB::createDefaultDistanceQuery()
{
  // Calculate distance to objects that were added dynamically into the planning scene
  for (std::size_t j = 0; j < active_links_.size(); ++j)
  {
    DistanceQueryData data;
    data.parent_name = active_links_[j]->getName(); 

    if (active_spheres_[j].size() == 0)
    {
      dist_query_.push_back(std::move(data));
      continue;
    }

    data.empty = false;

    // add active links
    for (std::size_t i = 0; i < active_links_.size() ; ++i)
    {
      if (i != j && !isCollisionAllowed(active_links_[i]->getName(), active_links_[j]->getName(), acm_.get()))
      {
        data.child_name.push_back(active_links_[i]->getName());
        data.child_index.push_back(i);
        data.child_type.push_back(Active);
      }
    }

    // add dynamic links
    for (std::size_t i = 0; i < dynamic_links_.size() ; ++i)
    {
      if (!isCollisionAllowed(dynamic_links_[i]->getName(), active_links_[j]->getName(), acm_.get()))
      {
        data.child_name.push_back(dynamic_links_[i]->getName());
        data.child_index.push_back(i);
        data.child_type.push_back(Dynamic);
      }
    }

    // add static links
    for (std::size_t i = 0; i < static_links_.size() ; ++i)
    {
      if (!isCollisionAllowed(static_links_[i]->getName(), active_links_[j]->getName(), acm_.get()))
      {
        data.child_name.push_back(static_links_[i]->getName());
        data.child_index.push_back(i);
        data.child_type.push_back(Static);
      }
    }

    dist_query_.push_back(std::move(data));
  }
}

void distance_field::CollisionRobotOpenVDB::distanceSelf(const collision_detection::DistanceRequest &req, collision_detection::DistanceResult &res, const moveit::core::RobotState &state) const
{
  std::vector<DistanceQueryData> dist_query(dist_query_);
  std::vector<std::vector<SDFData> > data(3);

  data[Static].reserve(static_links_.size());
  data[Dynamic].reserve(dynamic_links_.size());
  data[Active].reserve(active_links_.size());

  // collecting group links
  const moveit::core::JointModelGroup* group = robot_model_->getJointModelGroup(req.group_name);
  const auto& group_links = group->getUpdatedLinkModelsWithGeometryNames();

  for (std::size_t i = 0; i < active_links_.size(); ++i)
  {
    openvdb::Mat4d tf;
    Affine3dToMat4d(state.getGlobalLinkTransform(active_links_[i]), tf);
    dist_query[i].spheres = active_spheres_[i];
    dist_query[i].gradient = req.gradient;

    // transform sphere origins into world coordinate system
    std::transform(dist_query[i].spheres.begin(), dist_query[i].spheres.end(), dist_query[i].spheres.begin(),
                   [&tf](std::pair<openvdb::math::Vec3d, double> p){ p.first = (tf * p.first); return p; });

    // NOTE that we transform spheres before transposing 'tf'. OpenVDB is using a different convention than
    // we are (I guess?)
    tf = tf.transpose();
    SDFData d(active_sdf_[i]->getGrid(), tf);
    data[Active].push_back(d);
  }

  for (std::size_t i = 0; i < dynamic_links_.size(); ++i)
  {
    openvdb::Mat4d tf;
    Affine3dToMat4dAffine(state.getGlobalLinkTransform(dynamic_links_[i]), tf);

    SDFData d(dynamic_sdf_[i]->getGrid(), tf);
    data[Dynamic].push_back(d);
  }

  for (std::size_t i = 0; i < static_links_.size(); ++i)
  {
    SDFData d(static_sdf_[i]->getGrid());
    data[Static].push_back(d);
  }

  // Compute minimum distance
  for (std::size_t i = 0 ; i < dist_query.size() ; ++i)
  {
    if (!dist_query[i].empty)
    {
      collision_detection::DistanceResultsData d;
      distanceSelfHelper(dist_query[i], data, d);
      res.distance.insert(std::make_pair(d.link_name[0], d));
    }
  }

  double d = background_;
  std::string index = res.distance.begin()->second.link_name[0];
  for (auto it = res.distance.begin(); it != res.distance.end(); ++it)
  {
    if (it->second.min_distance < d)
    {
      index = it->second.link_name[0];
      d = it->second.min_distance;
    }
  }
  res.minimum_distance = res.distance[index];
}

bool distance_field::CollisionRobotOpenVDB::isCollisionAllowed(const std::string &l1, const std::string &l2, const collision_detection::AllowedCollisionMatrix *acm) const
{
    // use the collision matrix (if any) to avoid certain distance checks
    if (acm)
    {
      collision_detection::AllowedCollision::Type type;

      bool found = acm->getAllowedCollision(l1, l2, type);
      if (found)
      {
        // if we have an entry in the collision matrix, we read it
        return type == collision_detection::AllowedCollision::ALWAYS;
      }
    }
    return false;
}

void distance_field::CollisionRobotOpenVDB::createDefaultAllowedCollisionMatrix()
{
  acm_.reset(new collision_detection::AllowedCollisionMatrix());
  // Use default collision operations in the SRDF to setup the acm
  const std::vector<std::string>& collision_links = robot_model_->getLinkModelNamesWithCollisionGeometry();
  acm_->setEntry(collision_links, collision_links, false);

  // allow collisions for pairs that have been disabled
  const std::vector<srdf::Model::DisabledCollision> &dc = robot_model_->getSRDF()->getDisabledCollisionPairs();
  for (std::vector<srdf::Model::DisabledCollision>::const_iterator it = dc.begin(); it != dc.end(); ++it)
    acm_->setEntry(it->link1_, it->link2_, true);
}

template <typename T>
static bool approxEqual(T a, T b, const T eps = 0.00001)
{
  return std::abs(a - b) < eps;
}

void distance_field::CollisionRobotOpenVDB::distanceSelfHelper(const DistanceQueryData &data, std::vector<std::vector<SDFData> > &sdfs_data, collision_detection::DistanceResultsData &res) const
{
  res.min_distance = background_;
  res.link_name[0] = data.parent_name;
  res.hasNearestPoints = false;

  // Variables to keep track of gradient information, if requested
  openvdb::Vec3f gradient = openvdb::Vec3f::zero();
  float total_weights = 0.0f;

  for (std::size_t i = 0; i < data.child_index.size(); ++i)
  {
    float child_min = background_;
    openvdb::math::Coord child_min_ijk;
    bool dist_found = false;
    SDFData &child_data = sdfs_data[data.child_type[i]][data.child_index[i]];

    for (std::size_t j = 0; j < data.spheres.size(); ++j)
    {
      openvdb::math::Coord ijk = child_data.transform->worldToIndexNodeCentered(data.spheres[j].first);
      float child_dist = child_data.accessor.getValue(ijk);

      if (!approxEqual(child_dist, background_))
      {
        child_dist -= data.spheres[j].second;
        if (child_dist < child_min)
        {
          child_min = child_dist;
          child_min_ijk = ijk;
          dist_found = true;
        }
      }
    }

    if (dist_found)
    {
      // update link minimum distance
      if (child_min < res.min_distance)
      {
        res.min_distance = child_min;
        res.link_name[1] = data.child_name[i];
      }

      // compute gradient
      if (data.gradient)
      {
        // Compute gradient
        openvdb::Vec3f result = openvdb::math::ISGradient<openvdb::math::CD_2ND>::result(child_data.accessor, child_min_ijk);
        if (result.sum() != 0.0) // gradients can come back as [0, 0, 0]
        {
          float weight = background_ - child_min;
          total_weights += weight;
          result = child_data.transform->baseMap()->applyIJT(result);
          result.normalize();
          result = result * weight;
          gradient += result;
          res.hasGradient = true;
        }
      }
    }
  }

  if (res.hasGradient)
  {
    if (total_weights == 0.0)
    {
      res.gradient = Eigen::Vector3d(0, 0, 0);
    }
    else
    {
      res.gradient = Eigen::Vector3d(gradient(0) / total_weights, gradient(1) / total_weights, gradient(2) / total_weights);
      res.gradient.normalize();
    }
  }
}

distance_field::OpenVDBDistanceField::OpenVDBDistanceField(float voxel_size, float background) :
  voxel_size_(voxel_size),
  background_(background)
{
  openvdb::initialize();
  transform_ = openvdb::math::Transform::createLinearTransform(voxel_size_);
}

distance_field::OpenVDBDistanceField::OpenVDBDistanceField(openvdb::FloatGrid::Ptr grid)
{

}

openvdb::FloatGrid::Ptr distance_field::OpenVDBDistanceField::getGrid() const
{
  return grid_;
}

double distance_field::OpenVDBDistanceField::getVoxelSize() const
{
  return voxel_size_;
}

openvdb::math::Transform::Ptr distance_field::OpenVDBDistanceField::getTransform() const
{
  return transform_;
}

double distance_field::OpenVDBDistanceField::getDistance(const Eigen::Vector3f &point, bool thread_safe) const
{
  return getDistance(point(0), point(1), point(2), thread_safe);
}

double distance_field::OpenVDBDistanceField::getDistance(const openvdb::math::Coord &coord, bool thread_safe) const
{
  if (thread_safe)
  {
    return grid_->tree().getValue(coord);
  }

  if (accessor_)
  {
    return accessor_->getValue(coord);
  }
  else
  {
    ROS_ERROR("Tried to get distance data from and empty grid.");
    return 0;
  }
}

double distance_field::OpenVDBDistanceField::getDistance(const float &x, const float &y, const float &z, bool thread_safe) const
{
  openvdb::math::Vec3s xyz(x, y, z);
  return getDistance(transform_->worldToIndexNodeCentered(xyz), thread_safe);
}

bool distance_field::OpenVDBDistanceField::getGradient(const Eigen::Vector3f &point, Eigen::Vector3d &gradient, bool thread_safe) const
{
  return getGradient(point(0), point(1), point(2), gradient, thread_safe);
}

bool distance_field::OpenVDBDistanceField::getGradient(const openvdb::math::Coord &coord, Eigen::Vector3d &gradient, bool thread_safe) const
{
  openvdb::Vec3f result;
  if (thread_safe)
  {
    gradient(0) = (grid_->tree().getValue(coord.offsetBy(1, 0, 0)) - grid_->tree().getValue(coord.offsetBy(-1,  0,  0)))/(2*grid_->voxelSize()[0]);
    gradient(1) = (grid_->tree().getValue(coord.offsetBy(0, 1, 0)) - grid_->tree().getValue(coord.offsetBy( 0, -1,  0)))/(2*grid_->voxelSize()[1]);
    gradient(2) = (grid_->tree().getValue(coord.offsetBy(0, 0, 1)) - grid_->tree().getValue(coord.offsetBy( 0,  0, -1)))/(2*grid_->voxelSize()[2]);
  }
  else
  {
    if (accessor_)
    {
      result =  openvdb::math::ISGradient<openvdb::math::CD_2ND>::result(*accessor_, coord);
    }
    else
    {
      ROS_ERROR("Tried to get distance and gradient data from and empty grid.");
      return false;
    }

    gradient(0) = result(0);
    gradient(1) = result(1);
    gradient(2) = result(2);
  }

  if (gradient.norm() != 0)
  {
    gradient.normalize();
    return true;
  }
  else
  {
    return false;
  }
}

bool distance_field::OpenVDBDistanceField::getGradient(const float &x, const float &y, const float &z, Eigen::Vector3d &gradient, bool thread_safe) const
{
  openvdb::math::Vec3s xyz(x, y, z);
  return getGradient(transform_->worldToIndexNodeCentered(xyz), gradient, thread_safe);
}

void distance_field::OpenVDBDistanceField::fillWithSpheres(SphereModel &spheres, int maxSphereCount, bool overlapping, float minRadius, float maxRadius, float isovalue, int instanceCount)
{
  std::vector<openvdb::math::Vec4s> s;
  openvdb::tools::fillWithSpheres<openvdb::FloatGrid>(*grid_, s, maxSphereCount, overlapping, minRadius, maxRadius, isovalue, instanceCount);

  // convert data to eigen data types
  for (auto it = s.begin(); it != s.end(); ++it)
  {
    spheres.push_back(std::make_pair(it->getVec3(), (*it)[3]));
  }

  if (spheres.size()== 0)
    ROS_WARN("Unable to fill grid with spheres.");
}

void distance_field::OpenVDBDistanceField::addLinkToField(const moveit::core::LinkModel *link, const Eigen::Affine3d &pose, const float exBandWidth, const float inBandWidth)
{
  std::vector<shapes::ShapeConstPtr> shapes = link->getShapes();
  EigenSTL::vector_Affine3d shape_poses = link->getCollisionOriginTransforms();

  for (int j = 0; j < shapes.size(); ++j)
  {
    addShapeToField(shapes[j].get(), pose * shape_poses[j], exBandWidth, inBandWidth);
  }
}

void distance_field::OpenVDBDistanceField::addShapeToField(const shapes::Shape *shape, const Eigen::Affine3d &pose, const float exBandWidth, const float inBandWidth)
{
  openvdb::FloatGrid::Ptr grid;

  switch(shape->type)
  {
    case shapes::BOX:
    {
      const shapes::Box *box = static_cast<const shapes::Box *>(shape);
      const openvdb::math::Vec3s pmax = openvdb::math::Vec3s(std::abs(box->size[0])/2.0, std::abs(box->size[1])/2.0, std::abs(box->size[2])/2.0);
      const openvdb::math::Vec3s pmin = -1.0*pmax;

      std::vector<openvdb::math::Vec3s> points(8);
      points[0] = openvdb::math::Vec3s(pmin[0], pmin[1], pmin[2]);
      points[1] = openvdb::math::Vec3s(pmin[0], pmin[1], pmax[2]);
      points[2] = openvdb::math::Vec3s(pmax[0], pmin[1], pmax[2]);
      points[3] = openvdb::math::Vec3s(pmax[0], pmin[1], pmin[2]);
      points[4] = openvdb::math::Vec3s(pmin[0], pmax[1], pmin[2]);
      points[5] = openvdb::math::Vec3s(pmin[0], pmax[1], pmax[2]);
      points[6] = openvdb::math::Vec3s(pmax[0], pmax[1], pmax[2]);
      points[7] = openvdb::math::Vec3s(pmax[0], pmax[1], pmin[2]);

      std::vector<openvdb::Vec4I> quads(6);
      quads[0] = openvdb::Vec4I(0, 1, 2, 3); // bottom
      quads[1] = openvdb::Vec4I(7, 6, 5, 4); // top
      quads[2] = openvdb::Vec4I(4, 5, 1, 0); // front
      quads[3] = openvdb::Vec4I(6, 7, 3, 2); // back
      quads[4] = openvdb::Vec4I(0, 3, 7, 4); // left
      quads[5] = openvdb::Vec4I(1, 5, 6, 2); // right

      // Tranform point location
      TransformVec3s(pose, points.data(), points.size());

      // Convert data from world to index
      WorldToIndex(transform_, points.data(), points.size());

      openvdb::tools::QuadAndTriangleDataAdapter<openvdb::Vec3s, openvdb::Vec4I> mesh (points.data(), points.size(),
                                                                                       quads.data(), quads.size());
      grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, *transform_, exBandWidth, inBandWidth);
      break;
    }

    case shapes::CONE:
    {
      const shapes::Cone *cone = static_cast<const shapes::Cone *>(shape);

      // Number of sides
      int sides = std::ceil(2 * M_PI/(voxel_size_/cone->radius));

      // Cone Data
      double dtheta = 2 * M_PI/sides;
      double dh = cone->length/2.0;
      std::vector<openvdb::math::Vec3s> points(sides+2);
      std::vector<openvdb::Vec4I> quads(2*sides);

      // Create Vertices
      double theta = 0;
      for(int i = 0; i < sides; ++i)
      {
        double x = cone->radius * std::cos(theta);
        double y = cone->radius * std::sin(theta);
        points[i] = openvdb::math::Vec3s(x, y, -dh);
        theta += dtheta;
      }
      int top_pdx = sides;
      int bot_pdx = sides + 1;
      points[top_pdx] = openvdb::math::Vec3s(0.0, 0.0, dh);
      points[bot_pdx] = openvdb::math::Vec3s(0.0, 0.0, -dh);

      // Create polygons for wall and bottom
      for(int i = 0; i < sides; ++i)
      {
        int d = i;
        int d1 = i + 1;
        bool last = (i == (sides-1));

        if (last)
          d1 = 0;

        // Create wall
        quads[i] = openvdb::Vec4I(top_pdx, d, d1, openvdb::util::INVALID_IDX);

        // Create bottom cap
        quads[sides+i] = openvdb::Vec4I(bot_pdx, d1, d, openvdb::util::INVALID_IDX);
      }

      // Transform point location
      TransformVec3s(pose, points.data(), points.size());

      // Convert data from world to index
      WorldToIndex(transform_, points.data(), points.size());

      ROS_WARN("Cone");
      openvdb::tools::QuadAndTriangleDataAdapter<openvdb::Vec3s, openvdb::Vec4I> mesh (points.data(), points.size(),
                                                                                       quads.data(), quads.size());
      grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, *transform_, exBandWidth, inBandWidth);

      break;
    }

    case shapes::CYLINDER:
    {
      const shapes::Cylinder *cylinder = static_cast<const shapes::Cylinder *>(shape);

      // Number of sides
      int sides = std::ceil(2 * M_PI/(voxel_size_/cylinder->radius));

      // Cylinder Precision
      double dtheta = 2 * M_PI/sides;

      double dh = cylinder->length/2.0;
      std::vector<openvdb::math::Vec3s> points(2*(sides+1));
      std::vector<openvdb::Vec4I> quads(3*sides);

      // Create Vertices
      double theta = 0;
      int sign = 1;
      for(int i = 0; i < sides; ++i)
      {
        double x = cylinder->radius * std::cos(theta);
        double y = cylinder->radius * std::sin(theta);
        points[2 * i] = openvdb::math::Vec3s(x, y, sign * dh);
        points[(2 * i)+1] = openvdb::math::Vec3s(x, y, -sign * dh);
        theta += dtheta;
        sign = -1 * sign;
      }
      int top_pdx = (2 * sides);
      int bot_pdx = (2 * sides) + 1;
      points[top_pdx] = openvdb::math::Vec3s(0.0, 0.0, dh);
      points[bot_pdx] = openvdb::math::Vec3s(0.0, 0.0, -dh);

      // Create polygons for wall and caps
      for(int i = 0; i < sides; ++i)
      {
        int d = i * 2;
        int d1 = d + 1;
        int d2 = d + 2;
        int d3 = d + 3;

        bool last = (i == (sides-1));
        if( i % 2 == 0)
        {
          if (last)
          {
            d2 = 1;
            d3 = 0;
          }

          // Create wall
          quads[i] = openvdb::Vec4I(d, d1, d2, d3);

          // Create top cap
          quads[sides+i] = openvdb::Vec4I(top_pdx, d, d3, openvdb::util::INVALID_IDX);

          // Create bottom cap
          quads[(2*sides)+i] = openvdb::Vec4I(bot_pdx, d1, d2, openvdb::util::INVALID_IDX);
        }
        else
        {
          if (last)
          {
            d2 = 0;
            d3 = 1;
          }

          // Create wall
          quads[i] = openvdb::Vec4I(d3, d2, d1, d);

          // Create top cap
          quads[sides+i] = openvdb::Vec4I(top_pdx, d1, d2, openvdb::util::INVALID_IDX);

          // Create bottom cap
          quads[(2*sides)+i] = openvdb::Vec4I(bot_pdx, d, d3, openvdb::util::INVALID_IDX);
        }
      }

      // Tranform point location
      TransformVec3s(pose, points.data(), points.size());

      // Convert data from world to index
      WorldToIndex(transform_, points.data(), points.size());

      openvdb::tools::QuadAndTriangleDataAdapter<openvdb::Vec3s, openvdb::Vec4I> mesh (points.data(), points.size(),
                                                                                       quads.data(), quads.size());
      grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh, *transform_, exBandWidth, inBandWidth);

      break;
    }

    case shapes::OCTREE:
    {
      ROS_ERROR("OpenVDB Distance Field: Shape Type OCTREE is not implemented.");
      return;
    }

    case shapes::MESH:
    {
      shapes::Mesh *mesh = static_cast<shapes::Mesh *>(shape->clone());

      // Now need to clean verticies
      mesh->mergeVertices(0.0001);

      // Get points and triangles from Shape
      MeshData mesh_data = ShapeMeshToOpenVDB(mesh, pose);

      // Convert data from world to index
      WorldToIndex(transform_, mesh_data.points.data(), mesh_data.points.size());

      openvdb::tools::QuadAndTriangleDataAdapter<openvdb::Vec3s, openvdb::Vec3I> mesh2 (mesh_data.points.data(), mesh_data.points.size(),
                                                                                       mesh_data.triangles.data(), mesh_data.triangles.size());
      grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(mesh2, *transform_, exBandWidth, inBandWidth);

      delete mesh;
      break;
    }

    case shapes::PLANE:
    {
      ROS_ERROR("OpenVDB Distance Field: Shape Type PLANE is not implemented.");
      return;
    }

    case shapes::SPHERE:
    {
      ROS_WARN("SHPERE");
      const shapes::Sphere *sphere = static_cast<const shapes::Sphere *>(shape);

      grid = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(sphere->radius,
                                                                      openvdb::Vec3f(pose.translation()(0),
                                                                                     pose.translation()(1),
                                                                                     pose.translation()(2)),
                                                                      voxel_size_, exBandWidth);
      ROS_WARN("SPHERE DONE");
      break;
    }

    case shapes::UNKNOWN_SHAPE:
    {
      ROS_ERROR("OpenVDB Distance Field: Unknown Shape Type");
      return;
    }

  }

  if (!grid_)
  {
    grid_ = grid;
  }
  else
  {
    ros::Time start = ros::Time::now();
    openvdb::tools::csgUnion(*grid_, *grid, true);
    ROS_INFO("CSG Union Time Elapsed: %f (sec)",(ros::Time::now() - start).toSec());
  }

  accessor_ = std::make_shared<openvdb::FloatGrid::ConstAccessor>(grid_->getConstAccessor());
}

void distance_field::OpenVDBDistanceField::writeToFile(const std::string file_path)
{
  // Create a VDB file object.
  openvdb::io::File vdbFile(file_path);

  // Add the grid pointer to a container.
  openvdb::GridPtrVec grids;
  grids.push_back(grid_);

  // Write out the contents of the container.
  vdbFile.write(grids);
  vdbFile.close();
}

uint64_t distance_field::OpenVDBDistanceField::memUsage() const
{
  return grid_->memUsage();
}

distance_field::MeshData distance_field::ShapeMeshToOpenVDB(const shapes::Mesh *mesh, const Eigen::Affine3d &pose)
{
  MeshData output;
  openvdb::math::Mat4d tf;

  output.points.resize(mesh->vertex_count);
  output.triangles.resize(mesh->triangle_count);
  output.quads.resize(mesh->triangle_count);

  // Convert Eigen to OpenVDB
  Affine3dToMat4d(pose, tf);

  // Populate Verticies/Triangles/Quads
  for (unsigned int i = 0 ; i < mesh->triangle_count ; ++i)
  {
    unsigned int i3 = i * 3;
    unsigned int v1 = mesh->triangles[i3];
    unsigned int v2 = mesh->triangles[i3 + 1];
    unsigned int v3 = mesh->triangles[i3 + 2];
    openvdb::math::Vec3s p1(mesh->vertices[v1 * 3], mesh->vertices[v1 * 3 + 1], mesh->vertices[v1 * 3 + 2]);
    openvdb::math::Vec3s p2(mesh->vertices[v2 * 3], mesh->vertices[v2 * 3 + 1], mesh->vertices[v2 * 3 + 2]);
    openvdb::math::Vec3s p3(mesh->vertices[v3 * 3], mesh->vertices[v3 * 3 + 1], mesh->vertices[v3 * 3 + 2]);

    output.points[v1] = tf*p1;
    output.points[v2] = tf*p2;
    output.points[v3] = tf*p3;

    openvdb::Vec3I triangle(v1, v2, v3);
    output.triangles[i] = triangle;

    openvdb::Vec4I quad(v1, v2, v3, openvdb::util::INVALID_IDX);
    output.quads[i] = quad;
  }

  ROS_ASSERT(mesh->vertex_count == output.points.size());
  ROS_ASSERT(mesh->triangle_count == output.triangles.size());
  ROS_ASSERT(mesh->triangle_count == output.quads.size());

  return output;
}

void distance_field::Affine3dToMat4d(const Eigen::Affine3d &input, openvdb::math::Mat4d &output)
{
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      output(i, j) = input(i, j);

}

void distance_field::Affine3dToMat4dAffine(const Eigen::Affine3d &input, openvdb::math::Mat4d &output)
{
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      output(j, i) = input(i, j);
}

void distance_field::WorldToIndex(const openvdb::math::Transform::Ptr transform, std::vector<openvdb::math::Vec3s> &points)
{
  WorldToIndex(transform, points.data(), points.size());
}

void distance_field::TransformVec3s(const Eigen::Affine3d &pose, std::vector<openvdb::math::Vec3s> &points)
{
  TransformVec3s(pose, points.data(), points.size());
}

void distance_field::WorldToIndex(const openvdb::math::Transform::Ptr transform, openvdb::math::Vec3s *points, std::size_t size)
{
  std::transform(points, points + size, points,
                 [&transform](openvdb::math::Vec3s point){ return transform->worldToIndex(point); });
}

void distance_field::TransformVec3s(const Eigen::Affine3d &pose, openvdb::math::Vec3s *points, std::size_t size)
{
  openvdb::math::Mat4d tf;
  Affine3dToMat4d(pose, tf);
  std::transform(points, points + size, points,
                 [&tf](openvdb::math::Vec3s point){ return tf * point; });
}
