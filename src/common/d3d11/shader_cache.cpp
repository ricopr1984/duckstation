#include "shader_cache.h"
#include "../file_system.h"
#include "../log.h"
#include "../md5_digest.h"
#include <d3dcompiler.h>
Log_SetChannel(D3D11::ShaderCache);

namespace D3D11 {

#pragma pack(push, 1)
struct CacheIndexEntry
{
  u64 source_hash_low;
  u64 source_hash_high;
  u32 source_length;
  u32 shader_type;
  u32 file_offset;
  u32 blob_size;
};
#pragma pack(pop)

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache()
{
  if (m_index_file)
    std::fclose(m_index_file);
  if (m_blob_file)
    std::fclose(m_blob_file);
}

bool ShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
  return (source_hash_low == key.source_hash_low && source_hash_high == key.source_hash_high &&
          source_length == key.source_length && shader_type == key.shader_type);
}

bool ShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
  return (source_hash_low != key.source_hash_low || source_hash_high != key.source_hash_high ||
          source_length != key.source_length || shader_type != key.shader_type);
}

void ShaderCache::Open(std::string_view base_path, D3D_FEATURE_LEVEL feature_level, bool debug)
{
  m_feature_level = feature_level;
  m_debug = debug;

  const std::string base_filename = GetCacheBaseFileName(base_path, feature_level, debug);
  const std::string index_filename = base_filename + ".idx";
  const std::string blob_filename = base_filename + ".bin";

  if (!ReadExisting(index_filename, blob_filename))
    CreateNew(index_filename, blob_filename);
}

bool ShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
  if (FileSystem::FileExists(index_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing index file '%s'", index_filename.c_str());
    FileSystem::DeleteFile(index_filename.c_str());
  }
  if (FileSystem::FileExists(blob_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing blob file '%s'", blob_filename.c_str());
    FileSystem::DeleteFile(blob_filename.c_str());
  }

  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "wb");
  if (!m_index_file)
  {
    Log_ErrorPrintf("Failed to open index file '%s' for writing", index_filename.c_str());
    return false;
  }

  const u32 index_version = FILE_VERSION;
  if (std::fwrite(&index_version, sizeof(index_version), 1, m_index_file) != 1)
  {
    Log_ErrorPrintf("Failed to write version to index file '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "w+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Failed to open blob file '%s' for writing", blob_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  return true;
}

bool ShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
{
  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "r+b");
  if (!m_index_file)
    return false;

  u32 file_version;
  if (std::fread(&file_version, sizeof(file_version), 1, m_index_file) != 1 || file_version != FILE_VERSION)
  {
    Log_ErrorPrintf("Bad file version in '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "a+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Blob file '%s' is missing", blob_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  std::fseek(m_blob_file, 0, SEEK_END);
  const u32 blob_file_size = static_cast<u32>(std::ftell(m_blob_file));

  for (;;)
  {
    CacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
        (entry.file_offset + entry.blob_size) > blob_file_size)
    {
      if (std::feof(m_index_file))
        break;

      Log_ErrorPrintf("Failed to read entry from '%s', corrupt file?", index_filename.c_str());
      m_index.clear();
      std::fclose(m_blob_file);
      m_blob_file = nullptr;
      std::fclose(m_index_file);
      m_index_file = nullptr;
      return false;
    }

    const CacheIndexKey key{entry.source_hash_low, entry.source_hash_high, entry.source_length,
                            static_cast<ShaderCompiler::Type>(entry.shader_type)};
    const CacheIndexData data{entry.file_offset, entry.blob_size};
    m_index.emplace(key, data);
  }

  Log_InfoPrintf("Read %zu entries from '%s'", m_index.size(), index_filename.c_str());
  return true;
}

std::string ShaderCache::GetCacheBaseFileName(const std::string_view& base_path, D3D_FEATURE_LEVEL feature_level,
                                              bool debug)
{
  std::string base_filename(base_path);
  base_filename += FS_OSPATH_SEPERATOR_CHARACTER;
  base_filename += "d3d_shaders_";

  switch (feature_level)
  {
    case D3D_FEATURE_LEVEL_10_0:
      base_filename += "sm40";
      break;
    case D3D_FEATURE_LEVEL_10_1:
      base_filename += "sm41";
      break;
    case D3D_FEATURE_LEVEL_11_0:
      base_filename += "sm50";
      break;
    case D3D_FEATURE_LEVEL_11_1:
      base_filename += "sm51";
      break;
    case D3D_FEATURE_LEVEL_12_0:
      base_filename += "sm60";
      break;
    case D3D_FEATURE_LEVEL_12_1:
      base_filename += "sm61";
      break;
    default:
      base_filename += "unk";
      break;
  }

  if (debug)
    base_filename += "_debug";

  return base_filename;
}

ShaderCache::CacheIndexKey ShaderCache::GetCacheKey(ShaderCompiler::Type type, const std::string_view& shader_code)
{
  union
  {
    struct
    {
      u64 hash_low;
      u64 hash_high;
    };
    u8 hash[16];
  };

  MD5Digest digest;
  digest.Update(shader_code.data(), static_cast<u32>(shader_code.length()));
  digest.Final(hash);

  return CacheIndexKey{hash_low, hash_high, static_cast<u32>(shader_code.length()), type};
}

ShaderCache::ComPtr<ID3DBlob> ShaderCache::GetShaderBlob(ShaderCompiler::Type type, std::string_view shader_code)
{
  const auto key = GetCacheKey(type, shader_code);
  auto iter = m_index.find(key);
  if (iter == m_index.end())
    return CompileAndAddShaderBlob(key, shader_code);

  ComPtr<ID3DBlob> blob;
  HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.GetAddressOf());
  if (FAILED(hr) || std::fseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
      std::fread(blob->GetBufferPointer(), 1, iter->second.blob_size, m_blob_file) != iter->second.blob_size)
  {
    Log_ErrorPrintf("Read blob from file failed");
    return {};
  }

  return blob;
}

ShaderCache::ComPtr<ID3D11VertexShader> ShaderCache::GetVertexShader(ID3D11Device* device, std::string_view shader_code)
{
  ComPtr<ID3DBlob> blob = GetShaderBlob(ShaderCompiler::Type::Vertex, std::move(shader_code));
  if (!blob)
    return {};

  ComPtr<ID3D11VertexShader> vs;
  HRESULT hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, vs.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create vertex shader from blob: 0x%08X", hr);
    return {};
  }

  return vs;
}

ShaderCache::ComPtr<ID3D11GeometryShader> ShaderCache::GetGeometryShader(ID3D11Device* device,
                                                                         std::string_view shader_code)
{
  ComPtr<ID3DBlob> blob = GetShaderBlob(ShaderCompiler::Type::Geometry, std::move(shader_code));
  if (!blob)
    return {};

  ComPtr<ID3D11GeometryShader> gs;
  HRESULT hr =
    device->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, gs.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create geometry shader from blob: 0x%08X", hr);
    return {};
  }

  return gs;
}

ShaderCache::ComPtr<ID3D11PixelShader> ShaderCache::GetPixelShader(ID3D11Device* device, std::string_view shader_code)
{
  ComPtr<ID3DBlob> blob = GetShaderBlob(ShaderCompiler::Type::Pixel, std::move(shader_code));
  if (!blob)
    return {};

  ComPtr<ID3D11PixelShader> ps;
  HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create pixel shader from blob: 0x%08X", hr);
    return {};
  }

  return ps;
}

ShaderCache::ComPtr<ID3D11ComputeShader> ShaderCache::GetComputeShader(ID3D11Device* device,
                                                                       std::string_view shader_code)
{
  ComPtr<ID3DBlob> blob = GetShaderBlob(ShaderCompiler::Type::Compute, std::move(shader_code));
  if (!blob)
    return {};

  ComPtr<ID3D11ComputeShader> cs;
  HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, cs.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create compute shader from blob: 0x%08X", hr);
    return {};
  }

  return cs;
}

ShaderCache::ComPtr<ID3DBlob> ShaderCache::CompileAndAddShaderBlob(const CacheIndexKey& key,
                                                                   std::string_view shader_code)
{
  ComPtr<ID3DBlob> blob = ShaderCompiler::CompileShader(key.shader_type, m_feature_level, shader_code, m_debug);
  if (!blob)
    return {};

  if (!m_blob_file || std::fseek(m_blob_file, 0, SEEK_END) != 0)
    return blob;

  CacheIndexData data;
  data.file_offset = static_cast<u32>(std::ftell(m_blob_file));
  data.blob_size = static_cast<u32>(blob->GetBufferSize());

  CacheIndexEntry entry = {};
  entry.source_hash_low = key.source_hash_low;
  entry.source_hash_high = key.source_hash_high;
  entry.source_length = key.source_length;
  entry.shader_type = static_cast<u32>(key.shader_type);
  entry.blob_size = data.blob_size;
  entry.file_offset = data.file_offset;

  if (std::fwrite(blob->GetBufferPointer(), 1, entry.blob_size, m_blob_file) != entry.blob_size ||
      std::fflush(m_blob_file) != 0 || std::fwrite(&entry, sizeof(entry), 1, m_index_file) != 1 ||
      std::fflush(m_index_file) != 0)
  {
    Log_ErrorPrintf("Failed to write shader blob to file");
    return blob;
  }

  m_index.emplace(key, data);
  return blob;
}

} // namespace D3D11