#include "adt_c.h"

Indices32_t Adt_c::obj_uids_;
ModelMap_t Adt_c::model_map_;

Adt_c::Adt_c(Buffer_t *buffer, MpqHandler_c &mpq_h)
    : Chunk_c(buffer),
      mhdr_(this, 0xc),
      mcin_(this, mhdr_.mcin_off),
      mmdx_(this, mhdr_.mmdx_off),
      mmid_(this, mhdr_.mmid_off),
      mwmo_(this, mhdr_.mwmo_off),
      mwid_(this, mhdr_.mwid_off),
      mddf_(this, mhdr_.mddf_off),
      modf_(this, mhdr_.modf_off) {
  // check if we have to get MH2O chunk
  if (mhdr_.mh2o_off) {
    off_t mh2o_off = mhdr_.mh2o_off;
    mh2o_ = std::auto_ptr<Mh2oChunk_s>(new Mh2oChunk_s(this, mh2o_off));
  }

  InitMcnks();

  BuildTerrain(true);
  ParseDoodads(mpq_h, true); // true = use bounding volume, false = real mesh
  //ParseWmos(mpq_h);
}

Adt_c::~Adt_c() {
  for (int i = 0; i < 256; i++) {
    delete mcnks_[i];
  }
}

void Adt_c::InitMcnks() {
  mcnks_.reserve(256);
  for (int i = 0; i < 256; i++) {
    mcnks_.push_back(new McnkChunk_s(this, mcin_.mcnk_info[i].mcnk_off));
  }
}

void Adt_c::CleanUp() {
  for (ModelMap_t::iterator model = model_map_.begin();
       model != model_map_.end(); ++model) {
    delete model->second;
  }
  model_map_.clear();
}

void Adt_c::BuildTerrain(bool removeWet) {
  Points_t &vtx = terrain_.vtx;
  Points_t &norm = terrain_.norm;
  Indices32_t &idx = terrain_.idx;
  Indices32_t &col = terrain_.col;

  // reserve space for terrain
  vtx.reserve(256*145);
  norm.reserve(256*145);
  idx.reserve(256*768);

  // cycle through all mcnks and retrieve their geometry: 16*16 chunks
  for (McnkChunks_t::iterator mcnk = mcnks_.begin();
       mcnk != mcnks_.end();
       ++mcnk) {
    Points_t vertices, normals;
    Indices32_t indices;
    (*mcnk)->mcvt.GetVertices(&vertices);
    (*mcnk)->mcnr.GetNormals(&normals);
    (*mcnk)->mcvt.GetIndices(&indices);

    // merge all geometry information
    InsertIndices(indices, vtx.size(), &idx);
    vtx.insert(vtx.end(), vertices.begin(), vertices.end());
    norm.insert(norm.end(), normals.begin(), normals.end());
  }

  // STEP 1: Find all water cells in a terrain. Water is usually above terrain ;)
  //         so we just have to remove every terrain cell covered by water.

  // The concept behind all these nested loops is quite easy:
  // The first two loops will cycle through our 16*16 = 256 map chunks ..
  int wet_num = 0;
  if (removeWet && (mh2o_.get() != NULL)) {
    for (int y = 0; y < 16; y++) {
      for (int x = 0; x < 16; x++) {
        int chunk_idx = y*16+x; // this our chunk index
        // .. then we check if we have any water in this chunk at all ..
        for (uint32_t l = 0; l < mh2o_->content[chunk_idx].num_layers; l++) {
          // .. so we have water lets just get the water mask.
          const uint64_t &mask = mh2o_->content[chunk_idx].masks[l];
          // The mask is a 64bit value which has a bit set for every water cell.
          for (int wy = 0; wy < 8; wy++) {
            for (int wx = 0; wx < 8; wx++) {
              int cell_idx = wy*8+wx; // water cell index
              // Do we have water in this cell?
              if (mask & (1ULL << cell_idx)) {
                int terr_idx = y*16*64+x*64 + cell_idx; // terrain index
                // A terrain cell is made up of 12 indices which describe
                // 4 polygons that make up a cell in our map chunk.
                for (int i = 0; i < 12; i++) {
                  idx[terr_idx*12+i] = -1;  // mark every index with uint max
                  wet_num++;
                }
              }
            }
          }
        }
      }
    }
  } else {
    col.insert(col.end(), vtx.size(), 0xff127e14); // ABGR
    return;
  }

  // STEP 2: We have to remap indices and remove unecessary vertices
  //         corresponding to the indices we remove.
  //         Example:
  //          - Former index was 345, now its 0: idx_map[354] = 0;
  //          - Everytime former index 345 comes up we redirect it to 0
  //          - And everytime an index in idx_map is -1, push new vertex and
  //            assign idx_map[old_index] = dry_count;
  int dry_size = idx.size()-wet_num;  // how many indices are dry ones

  Indices32_t idx_map(idx.size(), 0xffffffff);    // indices remapped
  Indices32_t dry_idx(dry_size);                  // dry indices
  Indices32_t::iterator dry_it = dry_idx.begin();

  Points_t dry_vtx, dry_norm; // dry vertices and normals

  // this will mark all remaining terrain below ocean level (heigth=0)
  for (uint32_t i = 0; i < idx.size(); i++) {
    if (vtx[idx[i]].y < 0) {
      idx[(i/3)*3+0] = -1;
      idx[(i/3)*3+1] = -1;
      idx[(i/3)*3+2] = -1;
    }
  }

  int dry_count = 0;
  for (Indices32_t::iterator marked_terr = idx.begin();
       marked_terr != idx.end();
       ++marked_terr) {
    uint32_t marked_terrain = *marked_terr;
    // check if index is marked/wet
    if (marked_terrain != 0xffffffff) {
      // if not check for an already new index in the index map
      if (idx_map[marked_terrain] == 0xffffffff) {
        // we have a new index so map the new value and insert vtx and norms
        idx_map[*marked_terr] = dry_count;
        dry_vtx.push_back(vtx[marked_terrain]);
        dry_norm.push_back(norm[marked_terrain]);
        dry_count++;
      }
      // assign mapped index value to new index array
      *dry_it = idx_map[marked_terrain];
      ++dry_it;
    }
  }

  // assign cleaned up terrain
  vtx.swap(dry_vtx);
  norm.swap(dry_norm);
  idx.swap(dry_idx);
  col.insert(col.end(), vtx.size(), 0xff127e14); // ABGR
}

void Adt_c::ParseDoodads(MpqHandler_c &mpq_h, bool useCollisionModel) {

  for (McnkChunks_t::iterator mcnk = mcnks_.begin();
       mcnk != mcnks_.end();
       ++mcnk) {
    for (Indices32_t::iterator off = (*mcnk)->mcrf.doodad_offs.begin();
        off != (*mcnk)->mcrf.doodad_offs.end();
         ++off) {
      const MddfChunk_s::DoodadInfo_s &info = mddf_.doodad_infos.at(*off);
      // check if obj with unique id has already been placed
      if (CheckUid(info.uid)) { continue; }

      std::string filename(mmdx_.m2_names.c_str()+mmid_.name_offs.at(info.id));

      // replace false extensions with right one
      RreplaceWoWExt(ToLower(filename), ".mdx", ".m2", &filename);

      m2s_.push_back(Mesh_s());
      Mesh_s &m2_mesh = m2s_.back();

      // use collision (bounding volume) models or not
      M2_c *m2 = GetM2(mpq_h, filename, !useCollisionModel);
      if (useCollisionModel) {
        m2->GetBVMesh(&m2_mesh);
      } else {
        m2->GetMesh(*m2->skin, &m2_mesh);
      }
      TransWoWToRH(info.position, info.orientation, 1.0f, &m2_mesh.vtx);
    }
  }
}

M2_c* Adt_c::GetM2(MpqHandler_c &mpq_h, const std::string &filename, bool loadSkin) {
  // check if m2 with filename is in our map
  ModelMap_t::iterator found = model_map_.find(filename);
  if (found != model_map_.end()) {
    return reinterpret_cast<M2_c*>(found->second);
  } else {
    Buffer_t m2_buf;
    // not in map, so load it
    mpq_h.LoadFile(filename.c_str(), &m2_buf);
    if (!m2_buf.size()) {
      std::cout << filename << std::endl;
      return NULL;
    }

    // create new entry in map
    M2_c *m2 = new M2_c(&m2_buf);
    model_map_.insert(ModelPair_t(filename, m2));

    if (loadSkin) {
      std::string skin_name(ToLower(filename));
      RreplaceWoWExt(skin_name, ".m2", "00.skin", &skin_name);

      Buffer_t skin_buf;
      mpq_h.LoadFile(skin_name.c_str(), &skin_buf);
      Skin_c *skin = new Skin_c(&skin_buf);
      m2->skin = skin;
    }

    return m2;
  }
}

void Adt_c::ParseWmos(MpqHandler_c &mpq_h) {
  /*ModfChunk_s &modf = mhdr_.modf;
  MwidChunk_s &mwid = mhdr_.mwid;
  MwmoChunk_s &mwmo = mhdr_.mwmo;

  for (ModfChunk_s::WmoInfo_t::iterator wmo_info = modf.wmo_info.begin();
       wmo_info != modf.wmo_info.end();
       ++wmo_info) {
    // check if object with unique id has already been placed in our world
    Indices32_t::iterator found;
    found = std::find(obj_uids_.begin(), obj_uids_.end(), wmo_info->uid);
    if (found != obj_uids_.end()) { continue; }
    obj_uids_.push_back(wmo_info->uid);

    // get wmo filename
    std::string filename;
    filename = mwmo.wmo_names.c_str()+mwid.name_offsets.at(wmo_info->id);
    Wmo_c *wmo = GetWmo(mpq_h, filename);

    // create new wmo
    wmos_.push_back(Mesh_s());
    Mesh_s &wmo_mesh = wmos_.back();

    const Points_t &vtx = wmo->vertices();
    const Points_t &norm = wmo->normals();
    const Indices32_t &idx = wmo->indices();
    const Indices32_t &col = wmo->colors();

    wmo_mesh.vtx.assign(vtx.begin(), vtx.end());
    TransWoWToRH(wmo_info->position, wmo_info->orientation, 1.0f, &wmo_mesh.vtx);

    wmo_mesh.idx.assign(idx.begin(), idx.end());
    wmo_mesh.norm.assign(norm.begin(), norm.end());
    wmo_mesh.col.assign(col.begin(), col.end());
  }*/
}

/*Wmo_c* Adt_c::GetWmo(MpqHandler_c &mpq_h, const std::string &filename) {
  // check if wmo with filename is in our map ..
  ModelMap_t::iterator found = model_map_.find(filename);
  if (found != model_map_.end()) {
    return reinterpret_cast<Wmo_c*>(found->second);
  } else {
    Buffer_t wmo_buf;
    // .. not in map, so load it
    mpq_h.LoadFile(filename.c_str(), &wmo_buf);

    // init wmo and create new entry in map
    Wmo_c *wmo = new Wmo_c(&wmo_buf, filename, mpq_h);
    model_map_.insert(ModelPair_t(filename, wmo));
    return wmo;
  }
}*/

bool Adt_c::CheckUid(uint32_t uid) const {
  Indices32_t::iterator found;
  found = std::find(obj_uids_.begin(), obj_uids_.end(), uid);
  // find unique identifier ..
  if (found != obj_uids_.end()) {
    return true;
  } else {
    // .. push new uid cause we didn't see it until now
    obj_uids_.push_back(uid);
    return false;
  }
}

/*void Adt_c::BuildWater() {
  Mh2oChunk_s &mh2o = mhdr_.mh2o;
  if (mh2o.heights.size() <= 0) return;



  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      int chunk_idx = y*16+x;

      if (mh2o.heights[chunk_idx].val.size()) {
        Mh2oChunk_s::Mh2oHeights_s &heightmap = mh2o.heights[chunk_idx];
        glm::vec3 &pos = mhdr_.mcin.mcnks[chunk_idx].position;

        int hw = heightmap.w+1; // heights width (max. 9)
        int hh = heightmap.h+1; // heights height (max. 9)

        Points_t vertices(hw*hh);
        Points_t normals(hw*hh, glm::vec3(0, 1, 0));
        Indices32_t indices(heightmap.w*heightmap.h*6);

        //std::cout << std::hex << heightmap.mask << std::endl;
        for (uint32_t water_y = 0; water_y < heightmap.h; water_y++) {
          for (uint32_t water_x = 0; water_x < heightmap.w; water_x++) {
            // positions inside the chunk (8x8, offsets included)
            int cur_x = water_x+heightmap.x;
            int cur_y = water_y+heightmap.y;

            // the mask indicates where there's water to render
            if (heightmap.mask & (1ULL << (cur_y*8+cur_x))) {
              // indices for water heights
              int htl = hw*water_y+water_x;       // top left
              int htr = hw*water_y+water_x+1;     // top right
              int hbl = hw*(water_y+1)+water_x;   // bottom left
              int hbr = hw*(water_y+1)+water_x+1; // bottom right

              // width and depth values for our water planes
              float left = cur_x*TU*2-TU - pos.y;
              float right = cur_x*TU*2 + TU*2 - pos.y ;
              float top = cur_y*TU*2 - pos.x;
              float bottom = cur_y*TU*2 + TU*2 - pos.x;

              // actual vertices
              vertices[htl] = glm::vec3(left, heightmap.val.at(htl), top);
              vertices[htr] = glm::vec3(right, heightmap.val.at(htr), top);
              vertices[hbl] = glm::vec3(left, heightmap.val.at(hbl), bottom);
              vertices[hbr] = glm::vec3(right, heightmap.val.at(hbr), bottom);

              // 2*3 vertices -> 1 water quad
              uint32_t idx_pad = water_y*6*heightmap.w+water_x*6;
              indices.at(idx_pad+0) = htl;
              indices.at(idx_pad+1) = htr;
              indices.at(idx_pad+2) = hbr;
              indices.at(idx_pad+3) = hbr;
              indices.at(idx_pad+4) = hbl;
              indices.at(idx_pad+5) = htl;
            }
          }
        }

        InsertIndices(indices, water_.vtx.size(), &water_.idx);
        water_.vtx.insert(water_.vtx.end(), vertices.begin(), vertices.end());
        water_.norm.insert(water_.norm.end(), normals.begin(), normals.end());
        water_.col.insert(water_.col.end(), vertices.size(), 0xffff0000);
      }
    }
  }
}*/
