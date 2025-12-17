#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

static void die(const std::string& msg)
{
  throw std::runtime_error(msg);
}


// helper: clamp float to [0..255] with the required mapping
static uint8_t quant01_to_u8_minus1_plus1(float x)
{
  x = std::max(-1.0f, std::min(1.0f, x));
  // [255 * 0.5 * (x + 1)]
  float v = 255.0f * 0.5f * (x + 1.0f);
  int iv = static_cast<int>(std::lround(v));
  iv = std::max(0, std::min(255, iv));
  return static_cast<uint8_t>(iv);
}

// Read accessor as floats (handles component types + normalization).
// Returns a flat array: count * expectedComps floats.
static std::vector<float> readAccessorFloats(const tinygltf::Model& model, int accessorIndex, int expectedComps)
{
  if (accessorIndex < 0)
    return {};

  const auto& acc = model.accessors.at(accessorIndex);
  const auto& bv  = model.bufferViews.at(acc.bufferView);
  const auto& buf = model.buffers.at(bv.buffer);

  const size_t count = acc.count;
  std::vector<float> out(count * size_t(expectedComps), 0.0f);

  const size_t comps = tinygltf::GetNumComponentsInType(acc.type);
  if (int(comps) < expectedComps)
    die("Accessor has fewer components than expected");

  const size_t compSize = tinygltf::GetComponentSizeInBytes(acc.componentType);
  const size_t stride =
    bv.byteStride != 0 ? bv.byteStride : compSize * comps;

  const uint8_t* base =
    buf.data.data() + bv.byteOffset + acc.byteOffset;

  auto readOne = [&](const uint8_t* p, int compIdx) -> float {
    const uint8_t* pc = p + compIdx * compSize;
    switch (acc.componentType)
    {
      case TINYGLTF_COMPONENT_TYPE_FLOAT:
      {
        float v;
        std::memcpy(&v, pc, sizeof(float));
        return v;
      }
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
      {
        uint8_t v = *pc;
        if (acc.normalized)
          return float(v) / 255.0f;
        return float(v);
      }
      case TINYGLTF_COMPONENT_TYPE_BYTE:
      {
        int8_t v;
        std::memcpy(&v, pc, sizeof(int8_t));
        if (acc.normalized)
          return std::max(-1.0f, float(v) / 127.0f);
        return float(v);
      }
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
      {
        uint16_t v;
        std::memcpy(&v, pc, sizeof(uint16_t));
        if (acc.normalized)
          return float(v) / 65535.0f;
        return float(v);
      }
      case TINYGLTF_COMPONENT_TYPE_SHORT:
      {
        int16_t v;
        std::memcpy(&v, pc, sizeof(int16_t));
        if (acc.normalized)
          return std::max(-1.0f, float(v) / 32767.0f);
        return float(v);
      }
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
      {
        uint32_t v;
        std::memcpy(&v, pc, sizeof(uint32_t));
        return float(v);
      }
      default:
        die("Unsupported accessor componentType for float read");
        return 0.0f;
    }
  };

  for (size_t i = 0; i < count; ++i)
  {
    const uint8_t* p = base + i * stride;
    for (int c = 0; c < expectedComps; ++c)
      out[i * size_t(expectedComps) + size_t(c)] = readOne(p, c);
  }

  return out;
}

static std::vector<uint32_t> readIndicesU32(const tinygltf::Model& model, int accessorIndex)
{
  if (accessorIndex < 0)
    die("Primitive has no indices accessor");

  const auto& acc = model.accessors.at(accessorIndex);
  const auto& bv  = model.bufferViews.at(acc.bufferView);
  const auto& buf = model.buffers.at(bv.buffer);

  if (bv.byteStride != 0)
    die("Index bufferView must have byteStride == 0");

  const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;

  std::vector<uint32_t> out(acc.count);

  for (size_t i = 0; i < acc.count; ++i)
  {
    switch (acc.componentType)
    {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
      {
        uint16_t v;
        std::memcpy(&v, base + i * sizeof(uint16_t), sizeof(uint16_t));
        out[i] = uint32_t(v);
        break;
      }
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
      {
        uint32_t v;
        std::memcpy(&v, base + i * sizeof(uint32_t), sizeof(uint32_t));
        out[i] = v;
        break;
      }
      default:
        die("Unsupported index componentType (expected UNSIGNED_SHORT or UNSIGNED_INT)");
    }
  }

  return out;
}

// 32-byte baked vertex
struct BakedVertex32
{
  float    px, py, pz;   // 12
  uint8_t  nx, ny, nz;   // 3
  uint8_t  pad0;         // 1 => 16
  float    u, v;         // 8 => 24
  uint8_t  tx, ty, tz;   // 3
  uint8_t  tw;           // 1 => 28 (this is "1" required by glTF tangent.w)
  uint8_t  pad[4];       // 4 => 32
};

static_assert(sizeof(BakedVertex32) == 32, "BakedVertex32 must be 32 bytes");

// Copy everything except meshes/accessors/buffers; we will rebuild those
static void copySceneGraphAndMaterials(const tinygltf::Model& in, tinygltf::Model& out)
{
  out.asset    = in.asset;
  out.extensionsUsed = in.extensionsUsed;
  out.extensionsRequired = in.extensionsRequired;

  out.scenes   = in.scenes;
  out.defaultScene = in.defaultScene;

  out.nodes    = in.nodes;

  out.materials = in.materials;
  out.textures  = in.textures;
  out.images    = in.images;
  out.samplers  = in.samplers;

  // cameras/lights/etc if present
  out.cameras = in.cameras;
  out.lights  = in.lights;
}

static std::filesystem::path withSuffixBeforeExt(std::filesystem::path p, const std::string& suffix)
{
  auto stem = p.stem().string();
  auto ext  = p.extension().string();
  return p.parent_path() / (stem + suffix + ext);
}

static std::filesystem::path replaceExt(std::filesystem::path p, const std::string& newExt)
{
  p.replace_extension(newExt);
  return p;
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 2)
    {
      std::cerr << "Usage: model_bakery_baker <path/to/model.gltf|model.glb>\n";
      return 1;
    }

    std::filesystem::path inPath = argv[1];
    if (!std::filesystem::exists(inPath))
      die("Input file does not exist: " + inPath.string());

    tinygltf::TinyGLTF loader;
    tinygltf::Model inModel;

    std::string err, warn;
    bool ok = false;
    if (inPath.extension() == ".gltf")
      ok = loader.LoadASCIIFromFile(&inModel, &err, &warn, inPath.string());
    else if (inPath.extension() == ".glb")
      ok = loader.LoadBinaryFromFile(&inModel, &err, &warn, inPath.string());
    else
      die("Expected .gltf or .glb");

    if (!warn.empty())
      std::cerr << "tinygltf warning: " << warn << "\n";
    if (!ok)
      die("tinygltf failed: " + err);

    // Output paths
    std::filesystem::path outGltfPath = withSuffixBeforeExt(inPath, "_baked"); // .gltf/.glb same ext
    outGltfPath = replaceExt(outGltfPath, ".gltf"); // requirement says baked.gltf
    std::filesystem::path outBinPath  = outGltfPath;
    outBinPath.replace_extension(".bin"); // baked.bin

    // Build baked data
    std::vector<uint8_t> bakedVertexBytes;
    std::vector<uint8_t> bakedIndexBytes;

    struct RElem { uint32_t indexCount, indexOffset, vertexOffset; };
    std::vector<RElem> relems;
    std::vector<tinygltf::Mesh> outMeshes;

    // We'll rebuild meshes with new accessors/bufferviews
    outMeshes.reserve(inModel.meshes.size());

    // We'll need a mapping from original primitive -> new offsets.
    // We will bake each primitive independently (no vertex sharing between primitives).
    for (const auto& mesh : inModel.meshes)
    {
      tinygltf::Mesh outMesh;
      outMesh.name = mesh.name;

      outMesh.primitives.reserve(mesh.primitives.size());

      for (const auto& prim : mesh.primitives)
      {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES)
          continue;

        // Required POSITION
        auto itPos = prim.attributes.find("POSITION");
        if (itPos == prim.attributes.end())
          die("Primitive has no POSITION");

        const int accPos = itPos->second;

        // Optional
        const int accNor = (prim.attributes.count("NORMAL") ? prim.attributes.at("NORMAL") : -1);
        const int accTan = (prim.attributes.count("TANGENT") ? prim.attributes.at("TANGENT") : -1);
        const int accUv0 = (prim.attributes.count("TEXCOORD_0") ? prim.attributes.at("TEXCOORD_0") : -1);

        const auto& posAcc = inModel.accessors.at(accPos);
        const size_t vcount = posAcc.count;

        auto pos = readAccessorFloats(inModel, accPos, 3);
        std::vector<float> nor = (accNor >= 0) ? readAccessorFloats(inModel, accNor, 3) : std::vector<float>(vcount * 3, 0.0f);
        std::vector<float> uv  = (accUv0 >= 0) ? readAccessorFloats(inModel, accUv0, 2) : std::vector<float>(vcount * 2, 0.0f);

        // TANGENT in glTF is vec4. If missing, we'll set (1,0,0,1)
        std::vector<float> tan4;
        if (accTan >= 0)
          tan4 = readAccessorFloats(inModel, accTan, 4);
        else
        {
          tan4.resize(vcount * 4);
          for (size_t i = 0; i < vcount; ++i)
          {
            tan4[i*4 + 0] = 1.0f;
            tan4[i*4 + 1] = 0.0f;
            tan4[i*4 + 2] = 0.0f;
            tan4[i*4 + 3] = 1.0f;
          }
        }

        // Compute min/max for positions (for accessor)
        float pminx =  std::numeric_limits<float>::infinity();
        float pminy =  std::numeric_limits<float>::infinity();
        float pminz =  std::numeric_limits<float>::infinity();
        float pmaxx = -std::numeric_limits<float>::infinity();
        float pmaxy = -std::numeric_limits<float>::infinity();
        float pmaxz = -std::numeric_limits<float>::infinity();

        const uint32_t vertexOffset = uint32_t(bakedVertexBytes.size() / sizeof(BakedVertex32));

        bakedVertexBytes.resize(bakedVertexBytes.size() + vcount * sizeof(BakedVertex32));

        for (size_t i = 0; i < vcount; ++i)
        {
          const float px = pos[i*3 + 0];
          const float py = pos[i*3 + 1];
          const float pz = pos[i*3 + 2];

          pminx = std::min(pminx, px); pminy = std::min(pminy, py); pminz = std::min(pminz, pz);
          pmaxx = std::max(pmaxx, px); pmaxy = std::max(pmaxy, py); pmaxz = std::max(pmaxz, pz);

          const float nx = nor[i*3 + 0];
          const float ny = nor[i*3 + 1];
          const float nz = nor[i*3 + 2];

          const float u  = uv[i*2 + 0];
          const float v  = uv[i*2 + 1];

          const float tx = tan4[i*4 + 0];
          const float ty = tan4[i*4 + 1];
          const float tz = tan4[i*4 + 2];
          // tangent.w in glTF: we always store byte = 1 (requirement)
          (void)tan4[i*4 + 3];

          BakedVertex32 bv{};
          bv.px = px; bv.py = py; bv.pz = pz;
          bv.nx = quant01_to_u8_minus1_plus1(nx);
          bv.ny = quant01_to_u8_minus1_plus1(ny);
          bv.nz = quant01_to_u8_minus1_plus1(nz);
          bv.pad0 = 0;
          bv.u = u; bv.v = v;
          bv.tx = quant01_to_u8_minus1_plus1(tx);
          bv.ty = quant01_to_u8_minus1_plus1(ty);
          bv.tz = quant01_to_u8_minus1_plus1(tz);
          bv.tw = 1;
          bv.pad[0] = bv.pad[1] = bv.pad[2] = bv.pad[3] = 0;

          std::memcpy(
            bakedVertexBytes.data() + (vertexOffset + uint32_t(i)) * sizeof(BakedVertex32),
            &bv,
            sizeof(BakedVertex32));
        }

        // Indices -> uint32
        auto idx = readIndicesU32(inModel, prim.indices);

        const uint32_t indexOffset = uint32_t(bakedIndexBytes.size() / sizeof(uint32_t));
        const uint32_t indexCount  = uint32_t(idx.size());

        const size_t oldSz = bakedIndexBytes.size();
        bakedIndexBytes.resize(oldSz + idx.size() * sizeof(uint32_t));
        std::memcpy(bakedIndexBytes.data() + oldSz, idx.data(), idx.size() * sizeof(uint32_t));

        relems.push_back(RElem{ indexCount, indexOffset, vertexOffset });

        // Build a primitive placeholder; accessors will be filled later
        tinygltf::Primitive outPrim = prim;
        outPrim.attributes.clear();
        outPrim.indices = -1;
        outPrim.material = prim.material;
        outPrim.mode = prim.mode;
        outPrim.targets = prim.targets;
        outPrim.extras = prim.extras;
        outPrim.extensions = prim.extensions;

        // We'll temporarily store offsets using extras (safe)
        outPrim.extras = tinygltf::Value(tinygltf::Value::Object{
          {"_baked_vertexOffset", tinygltf::Value(int(vertexOffset))},
          {"_baked_vertexCount",  tinygltf::Value(int(vcount))},
          {"_baked_indexOffset",  tinygltf::Value(int(indexOffset))},
          {"_baked_indexCount",   tinygltf::Value(int(indexCount))},
          {"_baked_posMinX",      tinygltf::Value(double(pminx))},
          {"_baked_posMinY",      tinygltf::Value(double(pminy))},
          {"_baked_posMinZ",      tinygltf::Value(double(pminz))},
          {"_baked_posMaxX",      tinygltf::Value(double(pmaxx))},
          {"_baked_posMaxY",      tinygltf::Value(double(pmaxy))},
          {"_baked_posMaxZ",      tinygltf::Value(double(pmaxz))}
        });

        outMesh.primitives.push_back(std::move(outPrim));
      }

      outMeshes.push_back(std::move(outMesh));
    }

    // Build output glTF model
    tinygltf::Model outModel;
    copySceneGraphAndMaterials(inModel, outModel);
    outModel.meshes = std::move(outMeshes);

    // One buffer = baked bin
    tinygltf::Buffer outBuf;
    outBuf.name = "scene_baked";
    outBuf.data.reserve(bakedVertexBytes.size() + bakedIndexBytes.size());
    outBuf.data.insert(outBuf.data.end(), bakedVertexBytes.begin(), bakedVertexBytes.end());
    const size_t indexByteOffsetInBuffer = outBuf.data.size();
    outBuf.data.insert(outBuf.data.end(), bakedIndexBytes.begin(), bakedIndexBytes.end());
    outModel.buffers.clear();
    outModel.buffers.push_back(std::move(outBuf));

    // Two bufferViews: vertices + indices
    outModel.bufferViews.clear();

    tinygltf::BufferView vView;
    vView.buffer = 0;
    vView.byteOffset = 0;
    vView.byteLength = bakedVertexBytes.size();
    vView.byteStride = sizeof(BakedVertex32);
    vView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    vView.name = "baked_vertices";

    tinygltf::BufferView iView;
    iView.buffer = 0;
    iView.byteOffset = indexByteOffsetInBuffer;
    iView.byteLength = bakedIndexBytes.size();
    iView.byteStride = 0;
    iView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    iView.name = "baked_indices";

    outModel.bufferViews.push_back(vView);
    outModel.bufferViews.push_back(iView);

    // Create accessors for each primitive (POSITION, NORMAL, TEXCOORD_0, TANGENT, INDICES)
    outModel.accessors.clear();

    // Ensure extension in used list
    const std::string kExt = "KHR_mesh_quantization";
    auto& used = outModel.extensionsUsed;
    if (std::find(used.begin(), used.end(), kExt) == used.end())
      used.push_back(kExt);

    // Walk primitives and create accessors
    for (auto& mesh : outModel.meshes)
    {
      for (auto& prim : mesh.primitives)
      {
        auto obj = prim.extras.Get<tinygltf::Value::Object>();

        const int vertexOffset = obj["_baked_vertexOffset"].Get<int>();
        const int vertexCount  = obj["_baked_vertexCount"].Get<int>();
        const int indexOffset  = obj["_baked_indexOffset"].Get<int>();
        const int indexCount   = obj["_baked_indexCount"].Get<int>();

        const double pminx = obj["_baked_posMinX"].Get<double>();
        const double pminy = obj["_baked_posMinY"].Get<double>();
        const double pminz = obj["_baked_posMinZ"].Get<double>();
        const double pmaxx = obj["_baked_posMaxX"].Get<double>();
        const double pmaxy = obj["_baked_posMaxY"].Get<double>();
        const double pmaxz = obj["_baked_posMaxZ"].Get<double>();

        // POSITION accessor (float3) at offset 0
        tinygltf::Accessor accPos;
        accPos.bufferView = 0;
        accPos.byteOffset = size_t(vertexOffset) * sizeof(BakedVertex32) + 0;
        accPos.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accPos.count = size_t(vertexCount);
        accPos.type = TINYGLTF_TYPE_VEC3;
        accPos.normalized = false;
        accPos.minValues = {pminx, pminy, pminz};
        accPos.maxValues = {pmaxx, pmaxy, pmaxz};
        accPos.name = "POSITION_baked";
        const int accPosIdx = int(outModel.accessors.size());
        outModel.accessors.push_back(std::move(accPos));

        // NORMAL accessor (UNSIGNED_BYTE normalized, but viewer will interpret [0..1];
        // still OK for the assignment, renderer will decode back to [-1..1])
        tinygltf::Accessor accNor;
        accNor.bufferView = 0;
        accNor.byteOffset = size_t(vertexOffset) * sizeof(BakedVertex32) + 12;
        accNor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        accNor.count = size_t(vertexCount);
        accNor.type = TINYGLTF_TYPE_VEC3;
        accNor.normalized = true;
        accNor.name = "NORMAL_baked";
        // add quantization extension marker
        accNor.extensions[kExt] = tinygltf::Value(tinygltf::Value::Object{});
        const int accNorIdx = int(outModel.accessors.size());
        outModel.accessors.push_back(std::move(accNor));

        // TEXCOORD_0 accessor (float2) at offset 16
        tinygltf::Accessor accUv;
        accUv.bufferView = 0;
        accUv.byteOffset = size_t(vertexOffset) * sizeof(BakedVertex32) + 16;
        accUv.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accUv.count = size_t(vertexCount);
        accUv.type = TINYGLTF_TYPE_VEC2;
        accUv.normalized = false;
        accUv.name = "TEXCOORD_0_baked";
        const int accUvIdx = int(outModel.accessors.size());
        outModel.accessors.push_back(std::move(accUv));

        // TANGENT accessor: glTF expects vec4. We store xyz bytes at 24 and w=1 byte at 27.
        // We'll expose it as UNSIGNED_BYTE vec4 normalized with stride 32.
        tinygltf::Accessor accTan;
        accTan.bufferView = 0;
        accTan.byteOffset = size_t(vertexOffset) * sizeof(BakedVertex32) + 24;
        accTan.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        accTan.count = size_t(vertexCount);
        accTan.type = TINYGLTF_TYPE_VEC4;
        accTan.normalized = true;
        accTan.name = "TANGENT_baked";
        accTan.extensions[kExt] = tinygltf::Value(tinygltf::Value::Object{});
        const int accTanIdx = int(outModel.accessors.size());
        outModel.accessors.push_back(std::move(accTan));

        // INDICES accessor (uint32) in index bufferView
        tinygltf::Accessor accIdx;
        accIdx.bufferView = 1;
        accIdx.byteOffset = size_t(indexOffset) * sizeof(uint32_t);
        accIdx.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        accIdx.count = size_t(indexCount);
        accIdx.type = TINYGLTF_TYPE_SCALAR;
        accIdx.normalized = false;
        accIdx.name = "INDICES_baked";
        const int accIdxIdx = int(outModel.accessors.size());
        outModel.accessors.push_back(std::move(accIdx));

        prim.attributes["POSITION"]   = accPosIdx;
        prim.attributes["NORMAL"]     = accNorIdx;
        prim.attributes["TEXCOORD_0"] = accUvIdx;
        prim.attributes["TANGENT"]    = accTanIdx;
        prim.indices = accIdxIdx;

        // clean extras (remove our private keys)
        prim.extras = tinygltf::Value(); // empty
      }
    }

    // Output buffer URI
    outModel.buffers[0].uri = outBinPath.filename().string();

    // Write .bin
    {
      std::ofstream bin(outBinPath, std::ios::binary);
      if (!bin)
        die("Failed to create: " + outBinPath.string());
      const auto& bytes = outModel.buffers[0].data;
      if (!bytes.empty())
        bin.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }

    // Write .gltf
    tinygltf::TinyGLTF writer;
    writer.WriteGltfSceneToFile(
      &outModel,
      outGltfPath.string(),
      /*embedImages*/ false,
      /*embedBuffers*/ false,
      /*prettyPrint*/ true,
      /*writeBinary*/ false);

    std::cout << "Baked OK:\n";
    std::cout << "  " << outGltfPath << "\n";
    std::cout << "  " << outBinPath << "\n";
    return 0;
  }
  catch (const std::exception& e)
  {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
