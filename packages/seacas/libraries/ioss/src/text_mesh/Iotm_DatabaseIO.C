// Copyright(C) 1999-2020 National Technology & Engineering Solutions
// of Sandia, LLC (NTESS).  Under the terms of Contract DE-NA0003525 with
// NTESS, the U.S. Government retains certain rights in this software.
//
// See packages/seacas/LICENSE for details

#include "Iotm_DatabaseIO.h"

#include <Ioss_CodeTypes.h> // for Int64Vector, IntVector
#include <Ioss_SideBlock.h> // for SideBlock
#include <Ioss_Utils.h>     // for Utils, IOSS_ERROR
#include <fmt/ostream.h>

#include <algorithm> // for copy
#include <cassert>   // for assert
#include <cmath>     // for sqrt
#include <iostream>  // for ostringstream
#include <string>    // for string, operator==, etc
#include <utility>   // for pair

#include "Ioss_CommSet.h"      // for CommSet
#include "Ioss_DBUsage.h"      // for DatabaseUsage
#include "Ioss_DatabaseIO.h"   // for DatabaseIO
#include "Ioss_ElementBlock.h" // for ElementBlock
#include "Ioss_ElementTopology.h"
#include "Ioss_EntityType.h"     // for EntityType, etc
#include "Ioss_Field.h"          // for Field, etc
#include "Ioss_GroupingEntity.h" // for GroupingEntity
#include "Ioss_Hex8.h"
#include "Ioss_IOFactory.h"       // for IOFactory
#include "Ioss_Map.h"             // for Map, MapContainer
#include "Ioss_NodeBlock.h"       // for NodeBlock
#include "Ioss_NodeSet.h"         // for NodeSet
#include "Ioss_ParallelUtils.h"   // for ParallelUtils
#include "Ioss_Property.h"        // for Property
#include "Ioss_PropertyManager.h" // for PropertyManager
#include "Ioss_Region.h"          // for Region
#include "Ioss_SideSet.h"         // for SideSet
#include "Ioss_VariableType.h"    // for VariableType
#include "Iotm_TextMesh.h"        // for TextMesh

namespace {
  template <typename INT>
  void map_global_to_local(const Ioss::Map &map, size_t count, size_t stride, INT *data)
  {
    for (size_t i = 0; i < count; i += stride) {
      int64_t local = map.global_to_local(data[i], true);
      data[i]       = local;
    }
  }

  template <typename INT>
  void fill_transient_data(size_t component_count, double *data, INT *ids, size_t count,
                           double offset = 0.0)
  {
    if (component_count == 1) {
      for (size_t i = 0; i < count; i++) {
        data[i] = std::sqrt((double)ids[i]) + offset;
      }
    }
    else {
      for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < component_count; j++) {
          data[i * component_count + j] = j + std::sqrt((double)ids[i]) + offset;
        }
      }
    }
  }

  void fill_transient_data(const Ioss::GroupingEntity *entity, const Ioss::Field &field, void *data,
                           void *id_data, size_t count, double offset = 0.0)
  {
    const Ioss::Field &ids = entity->get_fieldref("ids");
    if (ids.is_type(Ioss::Field::INTEGER)) {
      fill_transient_data(field.raw_storage()->component_count(), reinterpret_cast<double *>(data),
                          reinterpret_cast<int *>(id_data), count, offset);
    }
    else {
      fill_transient_data(field.raw_storage()->component_count(), reinterpret_cast<double *>(data),
                          reinterpret_cast<int64_t *>(id_data), count, offset);
    }
  }
} // namespace
namespace Iotm {
  // ========================================================================
  const IOFactory *IOFactory::factory()
  {
    static IOFactory registerThis;
    return &registerThis;
  }

  IOFactory::IOFactory() : Ioss::IOFactory("textmesh") {}

  Ioss::DatabaseIO *IOFactory::make_IO(const std::string &filename, Ioss::DatabaseUsage db_usage,
                                       MPI_Comm                     communicator,
                                       const Ioss::PropertyManager &props) const
  {
    return new DatabaseIO(nullptr, filename, db_usage, communicator, props);
  }

  // ========================================================================
  DatabaseIO::DatabaseIO(Ioss::Region *region, const std::string &filename,
                         Ioss::DatabaseUsage db_usage, MPI_Comm communicator,
                         const Ioss::PropertyManager &props)
      : Ioss::DatabaseIO(region, filename, db_usage, communicator, props)
  {
    if (is_input()) {
      dbState = Ioss::STATE_UNKNOWN;
    }
    else {
      std::ostringstream errmsg;
      fmt::print(errmsg, "Text mesh option is only valid for input mesh.");
      IOSS_ERROR(errmsg);
    }
  }

  DatabaseIO::~DatabaseIO() { delete m_textMesh; }

  void DatabaseIO::read_meta_data__()
  {
    if (m_textMesh == nullptr) {
      if (get_filename() == "external") {
        std::ostringstream errmsg;
        fmt::print(errmsg, "ERROR: (text mesh) 'external' specified for mesh, but "
                           "set_text_mesh was not called to set the external mesh.\n");
        IOSS_ERROR(errmsg);
      }
      else {
        m_textMesh = new TextMesh(get_filename(), util().parallel_size(), util().parallel_rank());
      }
    }

    assert(m_textMesh != nullptr);

    Ioss::Region *this_region     = get_region();
    auto          glob_node_count = m_textMesh->node_count();
    auto          glob_elem_count = m_textMesh->element_count();

    this_region->property_add(Ioss::Property("global_node_count", glob_node_count));
    this_region->property_add(Ioss::Property("global_element_count", glob_elem_count));

    const int64_t two_billion = 2ll << 30;
    if ((glob_node_count > two_billion || glob_elem_count > two_billion) &&
        int_byte_size_api() == 4) {
      std::ostringstream errmsg;
      fmt::print(errmsg,
                 "ERROR: The node count is {:L} and the element count is {:L}.\n"
                 "       This exceeds the capacity of the 32-bit integers ({:L})\n"
                 "       which are being requested by the client.\n"
                 "       The mesh requires 64-bit integers which can be requested by setting the "
                 "`INTEGER_SIZE_API=8` property.",
                 glob_node_count, glob_elem_count, two_billion);
      IOSS_ERROR(errmsg);
    }

    spatialDimension  = m_textMesh->spatial_dimension();
    nodeCount         = m_textMesh->node_count_proc();
    elementCount      = m_textMesh->element_count_proc();
    elementBlockCount = m_textMesh->block_count();

    get_step_times__();

    add_transient_fields(this_region);
    get_nodeblocks();
    get_elemblocks();
    get_commsets();

    this_region->property_add(
        Ioss::Property(std::string("title"), std::string("TextMesh: ") += get_filename()));
  }

  bool DatabaseIO::begin__(Ioss::State /* state */) { return true; }

  bool DatabaseIO::end__(Ioss::State /* state */) { return true; }

  bool DatabaseIO::begin_state__(int /* state */, double time)
  {
    currentTime = time;
    return true;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::NodeBlock *nb, const Ioss::Field &field,
                                         void *data, size_t data_size) const
  {
    size_t num_to_get = field.verify(data_size);

    Ioss::Field::RoleType role = field.get_role();
    if (role == Ioss::Field::MESH) {
      if (field.get_name() == "mesh_model_coordinates") {
        // Cast 'data' to correct size -- double
        auto *rdata = static_cast<double *>(data);
        m_textMesh->coordinates(rdata);
      }
      else if (field.get_name() == "mesh_model_coordinates_x") {
        // Cast 'data' to correct size -- double
        auto *rdata = static_cast<double *>(data);
        m_textMesh->coordinates(1, rdata);
      }
      else if (field.get_name() == "mesh_model_coordinates_y") {
        // Cast 'data' to correct size -- double
        auto *rdata = static_cast<double *>(data);
        m_textMesh->coordinates(2, rdata);
      }
      else if (field.get_name() == "mesh_model_coordinates_z") {
        // Cast 'data' to correct size -- double
        auto *rdata = static_cast<double *>(data);
        m_textMesh->coordinates(3, rdata);
      }

      // NOTE: The implicit_ids field is ONLY provided for backward-
      // compatibility and should not be used unless absolutely
      // required. For text mesh, the implicit_ids and ids are the same.
      else if (field.get_name() == "ids" || field.get_name() == "implicit_ids") {
        // Map the local ids in this node block
        // (1...node_count) to global node ids.
        get_node_map().map_implicit_data(data, field, num_to_get, 0);
      }
      else if (field.get_name() == "owning_processor") {
        int *owner = static_cast<int *>(data);
        m_textMesh->owning_processor(owner, num_to_get);
      }
      else if (field.get_name() == "connectivity") {
        // Do nothing, just handles an idiosyncrasy of the GroupingEntity
      }
      else if (field.get_name() == "connectivity_raw") {
        // Do nothing, just handles an idiosyncrasy of the GroupingEntity
      }
      else {
        num_to_get = Ioss::Utils::field_warning(nb, field, "input");
      }
      return num_to_get;
    }

    const Ioss::Field &id_fld = nb->get_fieldref("ids");
    std::vector<char>  ids(id_fld.get_size());
    get_field_internal(nb, id_fld, ids.data(), id_fld.get_size());
    fill_transient_data(nb, field, data, ids.data(), num_to_get, currentTime);

    return num_to_get;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::Region * /*region*/, const Ioss::Field &field,
                                         void *data, size_t /*data_size*/) const
  {
    Ioss::Field::RoleType role = field.get_role();
    if (role == Ioss::Field::TRANSIENT) {
      // Fill the field with arbitrary data...
      (reinterpret_cast<double *>(data))[0] = static_cast<double>(rand());
    }
    return 1;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::ElementBlock *eb, const Ioss::Field &field,
                                         void *data, size_t data_size) const
  {
    size_t num_to_get = field.verify(data_size);

    int64_t               id            = eb->get_property("id").get_int();
    int64_t               element_count = eb->entity_count();
    Ioss::Field::RoleType role          = field.get_role();

    if (role == Ioss::Field::MESH) {
      // Handle the MESH fields required for an ExodusII file model.
      // (The 'genesis' portion)

      if (field.get_name() == "connectivity" || field.get_name() == "connectivity_raw") {
        assert(field.raw_storage()->component_count() == m_textMesh->topology_type(id).second);

        // The text mesh connectivity is returned in a vector.  Ids are global
        if (field.is_type(Ioss::Field::INTEGER)) {
          int *connect = static_cast<int *>(data);
          m_textMesh->connectivity(id, connect);
          if (field.get_name() == "connectivity_raw") {
            map_global_to_local(get_node_map(),
                                element_count * field.raw_storage()->component_count(), 1, connect);
          }
        }
        else {
          auto *connect = static_cast<int64_t *>(data);
          m_textMesh->connectivity(id, connect);
          if (field.get_name() == "connectivity_raw") {
            map_global_to_local(get_node_map(),
                                element_count * field.raw_storage()->component_count(), 1, connect);
          }
        }
      }
      else if (field.get_name() == "ids" || field.get_name() == "implicit_ids") {
        // Map the local ids in this element block
        // (eb_offset+1...eb_offset+1+element_count) to global element ids.
        get_element_map().map_implicit_data(data, field, num_to_get, eb->get_offset());
      }
      else {
        num_to_get = Ioss::Utils::field_warning(eb, field, "input");
      }
    }

    else if (role == Ioss::Field::ATTRIBUTE) {
      if (element_count > 0) {
        int attribute_count = eb->get_property("attribute_count").get_int();
        if (attribute_count > 0) {
          auto *attr = static_cast<double *>(data);
          for (size_t i = 0; i < num_to_get; i++) {
            attr[i] = 1.0;
          }
        }
      }
    }

    else if (role == Ioss::Field::TRANSIENT) {
      // Fill the field with arbitrary data...
      const Ioss::Field &id_fld = eb->get_fieldref("ids");
      std::vector<char>  ids(id_fld.get_size());
      get_field_internal(eb, id_fld, ids.data(), id_fld.get_size());
      fill_transient_data(eb, field, data, ids.data(), num_to_get, currentTime);
    }
    else if (role == Ioss::Field::REDUCTION) {
      num_to_get = Ioss::Utils::field_warning(eb, field, "input reduction");
    }
    return num_to_get;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::SideBlock *ef_blk, const Ioss::Field &field,
                                         void *data, size_t data_size) const
  {
    return -1;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::NodeSet *ns, const Ioss::Field &field,
                                         void *data, size_t data_size) const
  {
    return -1;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::EdgeBlock * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }
  int64_t DatabaseIO::get_field_internal(const Ioss::FaceBlock * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }
  int64_t DatabaseIO::get_field_internal(const Ioss::EdgeSet * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }
  int64_t DatabaseIO::get_field_internal(const Ioss::FaceSet * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }
  int64_t DatabaseIO::get_field_internal(const Ioss::ElementSet * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }

  int64_t DatabaseIO::get_field_internal(const Ioss::SideSet * /* fs */,
                                         const Ioss::Field & /* field */, void * /* data */,
                                         size_t /* data_size */) const
  {
    return -1;
  }
  int64_t DatabaseIO::get_field_internal(const Ioss::CommSet *cs, const Ioss::Field &field,
                                         void *data, size_t data_size) const
  {
    size_t num_to_get   = field.verify(data_size);
    size_t entity_count = cs->entity_count();
    assert(num_to_get == entity_count);

    // Return the <entity (node or face), processor> pair
    if (field.get_name() == "entity_processor" || field.get_name() == "entity_processor_raw") {
      // Check type -- node or face
      std::string type = cs->get_property("entity_type").get_string();

      if (type == "node") {
        // Allocate temporary storage space
        Ioss::Int64Vector entities(num_to_get);
        Ioss::IntVector   procs(num_to_get);
        m_textMesh->node_communication_map(entities, procs);

        // and store in 'data' ...
        if (field.is_type(Ioss::Field::INTEGER)) {
          int *entity_proc = static_cast<int *>(data);

          size_t j = 0;
          for (size_t i = 0; i < entity_count; i++) {
            assert(entities[i] > 0);
            entity_proc[j++] = entities[i];
            entity_proc[j++] = procs[i];
          }

          if (field.get_name() == "entity_processor_raw") {
            map_global_to_local(get_node_map(), 2 * entity_count, 2, entity_proc);
          }
        }
        else {
          auto *entity_proc = static_cast<int64_t *>(data);

          size_t j = 0;
          for (size_t i = 0; i < entity_count; i++) {
            assert(entities[i] > 0);
            entity_proc[j++] = entities[i];
            entity_proc[j++] = procs[i];
          }

          if (field.get_name() == "entity_processor_raw") {
            map_global_to_local(get_node_map(), 2 * entity_count, 2, entity_proc);
          }
        }
      }
      else {
        std::ostringstream errmsg;
        fmt::print(errmsg, "Invalid commset type {}", type);
        IOSS_ERROR(errmsg);
      }
    }
    else if (field.get_name() == "ids") {
      // Do nothing, just handles an idiosyncrasy of the GroupingEntity
    }
    else {
      num_to_get = Ioss::Utils::field_warning(cs, field, "input");
    }
    return num_to_get;
  }

  // Input only database -- these will never be called...
  int64_t DatabaseIO::put_field_internal(const Ioss::Region * /*reg*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::ElementBlock * /*eb*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::FaceBlock * /*nb*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::EdgeBlock * /*nb*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::NodeBlock * /*nb*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::ElementSet * /*ns*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::FaceSet * /*ns*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::EdgeSet * /*ns*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::NodeSet * /*ns*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::SideSet * /*fs*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::SideBlock * /*fb*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }
  int64_t DatabaseIO::put_field_internal(const Ioss::CommSet * /*cs*/,
                                         const Ioss::Field & /*field*/, void * /*data*/,
                                         size_t /*data_size*/) const
  {
    return -1;
  }

  const Ioss::Map &DatabaseIO::get_node_map() const
  {
    // Allocate space for node number map and read it in...
    // Can be called multiple times, allocate 1 time only
    if (nodeMap.map().empty()) {
      nodeMap.set_size(nodeCount);
      std::vector<int64_t> map;
      m_textMesh->node_map(map);
      nodeMap.set_map(map.data(), map.size(), 0, true);
    }
    return nodeMap;
  }

  const Ioss::Map &DatabaseIO::get_element_map() const
  {
    // Allocate space for element number map and read it in...
    // Can be called multiple times, allocate 1 time only
    if (elemMap.map().empty()) {
      elemMap.set_size(elementCount);
      std::vector<int64_t> map;
      m_textMesh->element_map(map);
      elemMap.set_map(map.data(), map.size(), 0, true);
    }
    return elemMap;
  }

  void DatabaseIO::get_nodeblocks()
  {
    std::string block_name = "nodeblock_1";
    auto        block =
        new Ioss::NodeBlock(this, block_name, m_textMesh->node_count_proc(), spatialDimension);
    block->property_add(Ioss::Property("id", 1));
    block->property_add(Ioss::Property("guid", util().generate_guid(1)));
    get_region()->add(block);
    add_transient_fields(block);
  }

  void DatabaseIO::get_step_times__()
  {
    int time_step_count = m_textMesh->timestep_count();
    for (int i = 0; i < time_step_count; i++) {
      get_region()->add_state(i);
    }
  }

  void DatabaseIO::get_elemblocks()
  {
    // Attributes of an element block are:
    // -- id
    // -- name
    // -- element type
    // -- number of elements
    // -- number of attributes per element
    // -- number of nodes per element (derivable from type)
    // -- number of faces per element (derivable from type)
    // -- number of edges per element (derivable from type)

    std::vector<std::string> blockNames = m_textMesh->get_part_names();
    int                      order      = 0;

    for (const std::string &name : blockNames) {
      int64_t     id            = m_textMesh->get_part_id(name);
      std::string type          = m_textMesh->topology_type(id).first;
      size_t      element_count = m_textMesh->element_count_proc(id);
      auto        block         = new Ioss::ElementBlock(this, name, type, element_count);

      block->property_add(Ioss::Property("id", id));
      block->property_add(Ioss::Property("guid", util().generate_guid(id)));
      block->property_add(Ioss::Property("original_block_order", order));

      block->property_add(Ioss::Property("global_entity_count", m_textMesh->element_count(id)));

      get_region()->add(block);
      add_transient_fields(block);

      order++;
    }
  }

  void DatabaseIO::get_commsets()
  {
    if (util().parallel_size() > 1) {
      // Get size of communication map...
      size_t my_node_count = m_textMesh->communication_node_count_proc();

      // Create a single node commset
      auto *commset = new Ioss::CommSet(this, "commset_node", "node", my_node_count);
      commset->property_add(Ioss::Property("id", 1));
      commset->property_add(Ioss::Property("guid", util().generate_guid(1)));
      get_region()->add(commset);
    }
  }

  unsigned DatabaseIO::entity_field_support() const
  {
    return Ioss::NODEBLOCK | Ioss::ELEMENTBLOCK | Ioss::REGION;
  }

  void DatabaseIO::add_transient_fields(Ioss::GroupingEntity *entity)
  {
    Ioss::EntityType type      = entity->type();
    size_t           var_count = m_textMesh->get_variable_count(type);
    for (size_t i = 0; i < var_count; i++) {
      std::string var_name = entity->type_string() + "_" + std::to_string(i + 1);
      entity->field_add(Ioss::Field(var_name, Ioss::Field::REAL, "scalar", Ioss::Field::TRANSIENT));
    }
  }
} // namespace Iotm
