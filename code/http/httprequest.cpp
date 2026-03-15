#include "httprequest.h"
#include "../pool/redisconnpool.h"
#include "../tool/md5.h"
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
using namespace std;




std::string HttpRequest::SafePath(const std::string& s) {
    for (char c : s) {
        if (!(c == '/' || c == '.' || c == '_' || c == '-' ||
              (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            throw std::invalid_argument("Unsafe character in path");
        }
    }
    return s;
}

struct Variant {
    std::string name;
    int width;
    int height;
    std::string bitrate;
    std::string audio_bitrate;
};

static const std::vector<Variant> kVariants = {
    {"360p", 640, 360, "800k", "96k"},
    {"720p", 1280, 720, "2000k", "128k"},
    {"1080p", 1920, 1080, "5000k", "192k"}
};

void HttpRequest::convertToHLSAsync(std::string input, std::string outputDir, std::string video_id) {
    std::thread([this, input = std::move(input), outputDir = std::move(outputDir), video_id = std::move(video_id)]() {
        try {
            std::string safeIn = SafePath(input);
            std::string safeOut = SafePath(outputDir);

            if (access(safeIn.c_str(), F_OK) != 0) {
                std::cerr << "[HLS] File not found: " << safeIn << "\n";
                // 转码失败，更新状态
                updateVideoStatus(video_id, false, "");
                std::remove(safeIn.c_str());
                return;
            }

            if (system(("mkdir -p " + safeOut).c_str()) != 0) {
                LOG_WARN("[HLS] mkdir failed for: %s", safeOut.c_str());
            }

            std::vector<std::string> variantPaths;

            for (const auto& var : kVariants) {
                std::string varDir = safeOut + "/" + var.name;
                if (system(("mkdir -p \"" + varDir + "\"").c_str()) != 0) {
                    LOG_WARN("[HLS] mkdir failed for: %s", varDir.c_str());
                    continue;
                }

                std::string segPattern = varDir + "/index%03d.ts";
                std::string playlist = varDir + "/index.m3u8";

                std::string vf = "scale=" + std::to_string(var.width) + ":" + std::to_string(var.height)
                               + ":force_original_aspect_ratio=decrease,"
                               + "pad=" + std::to_string(var.width) + ":" + std::to_string(var.height)
                               + ":(ow-iw)/2:(oh-ih)/2";

                std::string cmd =
                    "ffmpeg -y -i \"" + safeIn + "\" "
                    "-vf \"" + vf + "\" "
                    "-c:v libx264 -profile:v baseline -level 3.1 "
                    "-b:v " + var.bitrate + " -maxrate " + var.bitrate + " -bufsize " + var.bitrate + " "
                    "-c:a aac -b:a " + var.audio_bitrate + " -ar 44100 "
                    "-hls_time 4 -hls_list_size 0 "
                    "-hls_segment_filename \"" + segPattern + "\" "
                    "-f hls \"" + playlist + "\" "
                    "2>/dev/null";

                LOG_INFO("[HLS] Encoding %s", var.name.c_str());

                int ret = std::system(cmd.c_str());
                if (ret != 0) {
                    std::cerr << "[HLS] Failed to encode " << var.name << "\n";
                    continue;
                }
                variantPaths.push_back(var.name + "/index.m3u8");
            }

            if (!variantPaths.empty()) {
                std::string masterPath = safeOut + "/master.m3u8";
                std::ofstream master(masterPath);
                if (master.is_open()) {
                    master << "#EXTM3U\n";
                    master << "#EXT-X-VERSION:3\n\n";

                    for (size_t i = 0; i < variantPaths.size(); ++i) {
                        const auto& var = kVariants[i];
                        auto parseBitrate = [](const std::string& br) -> int {
                            std::string s = br;
                            if (s.back() == 'k' || s.back() == 'K') {
                                return std::stoi(s.substr(0, s.size()-1)) * 1000;
                            }
                            return std::stoi(s);
                        };
                        int totalBps = parseBitrate(var.bitrate) + parseBitrate(var.audio_bitrate);
                        std::string resolution = std::to_string(var.width) + "x" + std::to_string(var.height);

                        master << "#EXT-X-STREAM-INF:BANDWIDTH=" << totalBps
                               << ",RESOLUTION=" << resolution << "\n";
                        master << variantPaths[i] << "\n\n";
                    }
                    master.close();
                }
            }

            LOG_INFO("[HLS] Conversion completed.");
            
            // ==================== 步骤 5: HLS 转码完成后 UPDATE status='ready' ====================
            std::string final_hls_path = safeOut + "/master.m3u8";
            updateVideoStatus(video_id, true, final_hls_path);
            
            // 清理原始上传文件
            std::remove(safeIn.c_str());
            
        } catch (const std::exception& e) {
            std::cerr << "[HLS] Exception: " << e.what() << "\n";
            // 转码异常，更新状态为 failed
            updateVideoStatus(video_id, false, "");
        }
    }).detach();
}


// 网页名称，和一般的前端跳转不同，这里需要将请求信息放到后端来验证一遍再上传（和小组成员还起过争执）
const unordered_set<string> HttpRequest::DEFAULT_HTML {
    "/index", "/register", "/login", "/welcome", "/video", "/picture",
};

// 登录/注册
const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
    {"/login.html", 1}, {"/register.html", 0}
};

// 初始化操作，一些清零操作
void HttpRequest::Init() {
    state_ = REQUEST_LINE;  // 初始状态
    method_ = path_ = version_= body_ = "";
    header_.clear();
    post_.clear();
    content_length_ = 0;
    body_.clear();
}

// 解析处理
bool HttpRequest::parse(Buffer& buff) {
    const char END[] = "\r\n";
    if(buff.ReadableBytes() == 0)   // 没有可读的字节
        return false;
    // 读取数据开始
    while(buff.ReadableBytes() && state_ != FINISH) {
        // 从buff中的读指针开始到读指针结束，这块区域是未读取得数据并去处"\r\n"，返回有效数据得行末指针
        const char* lineend = search(buff.Peek(), buff.BeginWriteConst(), END, END+2);
        string line(buff.Peek(), lineend);
        switch (state_)
        {
        case REQUEST_LINE:
            // 解析错误
            if(!ParseRequestLine_(line)) {
                return false;
            }
            ParsePath_();   // 解析路径
            break;
        case HEADERS:
            ParseHeader_(line);
            if(buff.ReadableBytes() <= 2) {  // 说明是get请求，后面为\r\n
                state_ = FINISH;   // 提前结束
            }
            break;
        case BODY:
            ParseBody_(line);
            break;
        default:
            break;
        }
        if(lineend == buff.BeginWrite()) {  // 读完了
            buff.RetrieveAll();
            break;
        }
        buff.RetrieveUntil(lineend + 2);        // 跳过回车换行
    }
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}


bool HttpRequest::my_parse(Buffer& buff) {
    const char CRLF[] = "\r\n";
    
    while (buff.ReadableBytes() > 0 && state_ != FINISH && state_ != ERROR) {
        // ==================== REQUEST_LINE & HEADERS ====================
        if (state_ == REQUEST_LINE || state_ == HEADERS) {
            const char* line_end = std::search(
                buff.Peek(), buff.BeginWriteConst(),
                CRLF, CRLF + 2
            );
            
            if (line_end == buff.BeginWriteConst()) {
                break; 
            }
            
            std::string line(buff.Peek(), line_end);
            buff.RetrieveUntil(line_end + 2);
            
            if (state_ == REQUEST_LINE) {
                if (!ParseRequestLine_(line)) return false;
                // 修复：complete 请求需要检查 path
                if (path_ == "/upload/complete") {
                    complete_signal_ = true;
                    state_ = BODY;
                    continue;
                }
                // 修复：秒传检查请求，添加 check_signal_ 标记以便区分
                if (path_ == "/upload/check") {
                    check_signal_ = true;
                    state_ = BODY;
                    continue;
                }
                if (method_ == "GET") {
                    state_ = FINISH;
                    return true;
                }
            } else if (state_ == HEADERS) {
                if (line.empty()) {
                    if (!header_.count("content-length")) {
                        // 修复：移除冗余的 complete_signal_ 设置（REQUEST_LINE 已设置）
                        if (path_ == "/upload/complete") {
                            state_ = BODY;
                        } else if (path_ == "/upload/check") {
                            state_ = BODY;
                        } else {
                            state_ = FINISH;
                        }
                        return true;
                    }
                    content_length_ = std::stoul(header_["content-length"]);
                    
                    // 修复：移除冗余的 complete_signal_ 设置（REQUEST_LINE 已设置）
                    if (path_ == "/upload/complete") {
                        state_ = BODY;
                    } else if (path_ == "/upload/check") {
                        state_ = BODY;
                    } else if (parseMultipartBoundary()) {
                        state_ = BODY_START;
                    } else {
                        state_ = FINISH;
                    }
                } else {
                    ParseHeader_(line);
                }
            }
        }
        // ==================== BODY_START ====================
        else if (state_ == BODY_START) {
            const char* line_end = std::search(
                buff.Peek(), buff.BeginWriteConst(),
                CRLF, CRLF + 2
            );
            if (line_end == buff.BeginWriteConst()) break;
            
            std::string line(buff.Peek(), line_end);
            buff.RetrieveUntil(line_end + 2);
            
            if (line == boundary_marker_) {
                state_ = BODY_DATA;
            } else {
                state_ = ERROR;
                return false;
            }
        }
        // ==================== BODY_DATA ====================
        else if (state_ == BODY_DATA) {
            if (!in_file_part_) {
                const char* line_end = std::search(
                    buff.Peek(), buff.BeginWriteConst(),
                    CRLF, CRLF + 2
                );
                if (line_end == buff.BeginWriteConst()) break;
                
                std::string line(buff.Peek(), line_end);
                buff.RetrieveUntil(line_end + 2);
                
                if (line.find("Content-Disposition") == 0) {
                    is_file_part_ = extractFilenameFromDisposition(line);
                }
                else if (line.find("X-File-ID:") == 0) {
                    file_id_ = line.substr(line.find(":") + 2);
                }
                else if (line.find("X-Chunk-Index:") == 0) {
                    chunk_index_ = std::stoi(line.substr(line.find(":") + 2));
                }
                else if (line.find("X-Total-Chunks:") == 0) {
                    total_chunks_ = std::stoi(line.substr(line.find(":") + 2));
                }
                else if (line.find("X-Chunk-MD5:") == 0) {
                    chunk_md5_ = line.substr(line.find(":") + 2);
                }
                
                if (line.empty()) {
                    if (is_file_part_) {
                        in_file_part_ = true;
                        
                        // 修复：分片上传前先检查该分片是否已存在（秒传）
                        if (!file_id_.empty() && chunk_index_ >= 0 && !chunk_md5_.empty()) {
                            if (checkChunkExistsByMD5(file_id_, chunk_index_, chunk_md5_)) {
                                LOG_INFO("[秒传] 分片已存在，跳过上传：video_id=%s, chunk=%d", 
                                         file_id_.c_str(), chunk_index_);
                                // 分片已存在，直接标记完成，更新 Redis Bitmap
                                updateRedisBitmap();
                                state_ = FINISH;
                                in_file_part_ = false;
                                continue;
                            }
                        }
                        
                        openChunkFile();
                    }
                }
            } else {
                // 流式写入数据
                const char* data = buff.Peek();
                size_t readable = buff.ReadableBytes();
                
                if (readable == 0) break;
                
                // 查找 closing boundary
                std::string searchStr = "\r\n" + boundary_end_;
                const char* closing_pos = std::search(
                    data, buff.BeginWriteConst(),
                    searchStr.begin(), searchStr.end()
                );
                
                if (closing_pos != buff.BeginWriteConst()) {
                    // 找到结束边界，写入之前的数据
                    size_t file_data_len = (closing_pos - data) - 2;
                    if (file_data_len > 0) {
                        video_file_.write(data, file_data_len);
                        body_received_ += file_data_len;
                    }
                    
                    buff.RetrieveUntil(closing_pos + searchStr.size());
                    video_file_.close();
                    in_file_part_ = false;
                    
                    if (!verifyChunkMD5()) {
                        state_ = ERROR;
                        return false;
                    }
                    
                    // 修复：MD5 验证成功后记录分片 MD5 映射
                    if (!file_id_.empty() && chunk_index_ >= 0 && !chunk_md5_.empty()) {
                        recordChunkMD5(file_id_, chunk_index_, chunk_md5_);
                    }
                    
                    updateRedisBitmap();
                    state_ = FINISH;
                } else {
                    // 未找到结束边界，写入所有数据
                    video_file_.write(data, readable);
                    body_received_ += readable;
                    buff.Retrieve(readable);
                }
            }
        }
        // ==================== BODY ====================
        // 修复：BODY 状态处理 complete 请求的 JSON body 和秒传检查
        else if (state_ == BODY) {
            if (path_ == "/upload/check") {
                // 秒传检查：解析 JSON 中的 file_id 和 total_chunks
                std::string json_body(buff.Peek(), buff.ReadableBytes());
                size_t readable = buff.ReadableBytes();
                
                if (readable >= content_length_) {
                    // 解析 JSON 中的 file_id 和 total_chunks
                    auto extractField = [](const std::string& json, const std::string& key) -> std::string {
                        size_t pos = json.find("\"" + key + "\":");
                        if (pos == std::string::npos) return "";
                        pos = json.find('"', pos + key.size() + 3);
                        if (pos == std::string::npos) return "";
                        size_t start = pos + 1;
                        size_t end = json.find('"', start);
                        if (end == std::string::npos) return "";
                        return json.substr(start, end - start);
                    };
                    
                    auto extractIntField = [](const std::string& json, const std::string& key) -> int {
                        size_t pos = json.find("\"" + key + "\":");
                        if (pos == std::string::npos) return -1;
                        size_t start = pos + key.size() + 3;
                        while (start < json.size() && isspace(json[start])) start++;
                        size_t end = start;
                        while (end < json.size() && (isdigit(json[end]) || json[end] == '-')) end++;
                        if (end == start) return -1;
                        return std::stoi(json.substr(start, end - start));
                    };
                    
                    // 修复：将解析结果存储到成员变量，供后续使用
                    file_id_ = extractField(json_body, "file_id");
                    total_chunks_ = extractIntField(json_body, "total_chunks");
                    
                    LOG_INFO("[秒传检查] video_id=%s, total_chunks=%d", file_id_.c_str(), total_chunks_);
                    
                    if (!file_id_.empty() && total_chunks_ > 0) {
                        // 检查分片上传完成状态
                        bool complete = checkFileUploadStatus(file_id_, total_chunks_);
                        
                        if (complete) {
                            LOG_INFO("[秒传检查] 文件已完整上传，可跳过：video_id=%s", file_id_.c_str());
                        } else {
                            LOG_INFO("[秒传检查] 文件未完整上传，继续上传：video_id=%s", file_id_.c_str());
                        }
                    }
                    
                    state_ = FINISH;
                    // 修复：确保数据被清除，避免重复处理
                    buff.Retrieve(readable);
                    // 修复：重置 check_signal_ 标志
                    check_signal_ = false;
                } else {
                    break;
                }
            }
            else if (path_ == "/upload/complete") {
                // 累积 body 数据直到收到完整内容
                size_t readable = buff.ReadableBytes();
                if (readable >= content_length_) {
                    // body 已完整，保持数据在 buff 中供后续处理
                    state_ = FINISH;
                } else {
                    // 继续等待更多数据
                    break;
                }
            } else {
                // 原有 ParseBody_ 逻辑
                const char* line_end = std::search(
                    buff.Peek(), buff.BeginWriteConst(),
                    CRLF, CRLF + 2
                );
                if (line_end != buff.BeginWriteConst()) {
                    std::string line(buff.Peek(), line_end);
                    ParseBody_(line);
                    buff.RetrieveUntil(line_end + 2);
                } else {
                    break;
                }
            }
        }
        // ==================== BODY_END ====================
        else if (state_ == BODY_END) {
            const char* line_end = std::search(
                buff.Peek(), buff.BeginWriteConst(),
                CRLF, CRLF + 2
            );
            if (line_end == buff.BeginWriteConst()) break;
            
            buff.RetrieveUntil(line_end + 2);
            state_ = FINISH;
            video_file_.close();
        }
    }
    
    // 处理完成请求
    if (state_ == FINISH && complete_signal_) {
        // 重置标志，允许后续请求
        complete_signal_ = false;
        
        std::string json_body(buff.Peek(), buff.ReadableBytes());
        
        std::string upload_id, filename;
        int total_chunks = -1;

        auto extractField = [](const std::string& json, const std::string& key) -> std::string {
            size_t pos = json.find("\"" + key + "\":");
            if (pos == std::string::npos) return "";
            pos = json.find('"', pos + key.size() + 3);
            if (pos == std::string::npos) return "";
            size_t start = pos + 1;
            size_t end = json.find('"', start);
            if (end == std::string::npos) return "";
            return json.substr(start, end - start);
        };

        auto extractIntField = [](const std::string& json, const std::string& key) -> int {
            size_t pos = json.find("\"" + key + "\":");
            if (pos == std::string::npos) return -1;
            size_t start = pos + key.size() + 3;
            while (start < json.size() && isspace(json[start])) start++;
            size_t end = start;
            while (end < json.size() && (isdigit(json[end]) || json[end] == '-')) end++;
            if (end == start) return -1;
            return std::stoi(json.substr(start, end - start));
        };

        // 修复：使用 header 中的 file_id 作为 upload_id，保持一致性
        upload_id = extractField(json_body, "file_id");
        filename = extractField(json_body, "filename");
        total_chunks = extractIntField(json_body, "total_chunks");
        
        if (upload_id.empty() || total_chunks <= 0) {
            LOG_ERROR("Invalid complete request parameters");
            download_in_progress_ = false;
            return false;
        }
        
        // ==================== 新增：获取本地内存锁（防止并发合并）====================
        // 修复：单机部署不需要分布式锁，使用本地 mutex 即可
        {
            std::lock_guard<std::mutex> lock(upload_mutex_);
            
            // 检查是否已在上传中（防止重复处理）
            if (uploading_files_.count(upload_id) > 0) {
                LOG_WARN("[Local Lock] Upload already in progress for upload_id: %s", upload_id.c_str());
                download_in_progress_ = false;
                return true;  // 返回成功，避免客户端重试
            }
            uploading_files_.insert(upload_id);
        }
        
        // 使用 RAII 确保锁一定释放
        struct LockGuard {
            std::string& id;
            LockGuard(std::string& upload_id) : id(upload_id) {}
            ~LockGuard() {
                std::lock_guard<std::mutex> lock(upload_mutex_);
                uploading_files_.erase(id);
                LOG_INFO("[Local Lock] Released for upload_id: %s", id.c_str());
            }
        } lock_guard(upload_id);
        
        LOG_INFO("[Local Lock] Lock acquired for upload_id: %s", upload_id.c_str());
        
        // 修复：使用 upload_id 作为 chunk_dir，与 openChunkFile 一致
        std::string chunk_dir = "./sever_videodata/" + upload_id;
        std::string output_path = "./sever_videodata/" + upload_id + "/merged_" + filename;
        
        LOG_INFO("Combining chunks: %s, %s, %d", 
                 upload_id.c_str(), filename.c_str(), total_chunks);
        
        // ==================== 步骤 1: 先插入数据库 (status='uploading') ====================
        MYSQL* sql = nullptr;
        bool transaction_started = false;
        bool db_insert_success = false;
        
        // 修复：生成 video_id（使用时间戳 + 随机数）
        std::string video_id = "vid_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(rand());
        std::string hls_path = "./hls/" + upload_id;
        
        {
            SqlConnRAII raii(&sql, SqlConnPool::Instance());
            if (sql) {
                // 开始事务
                if (beginTransaction(sql)) {
                    transaction_started = true;
                    
                    try {
                        // 安全转义字符串（防 SQL 注入）
                        auto escape = [sql](const std::string& s) -> std::string {
                            if (s.empty()) return "";
                            std::string res;
                            res.resize(s.size() * 2 + 1);
                            unsigned long len = mysql_real_escape_string(sql, &res[0], s.c_str(), s.size());
                            res.resize(len);
                            return res;
                        };
                        
                        std::string escaped_id = escape(video_id);
                        std::string escaped_name = escape(filename);
                        std::string escaped_hls = escape(hls_path);
                        std::string escaped_upload_id = escape(upload_id);
                        
                        // 修复：先检查 upload_id 是否已存在（防止重复插入）
                        std::string check_sql = 
                            "SELECT id FROM videos WHERE upload_id = '" + escaped_upload_id + "' LIMIT 1";
                        if (mysql_query(sql, check_sql.c_str()) == 0) {
                            MYSQL_RES* res = mysql_store_result(sql);
                            if (res && mysql_num_rows(res) > 0) {
                                // upload_id 已存在，说明之前已处理过
                                LOG_WARN("[Transaction] upload_id already exists: %s", upload_id.c_str());
                                mysql_free_result(res);
                                rollbackTransaction(sql);
                                download_in_progress_ = false;
                                return true;  // 返回成功，避免客户端重试
                            }
                            mysql_free_result(res);
                        }
                        
                        // 修复：插入视频记录时同时保存 upload_id 关联
                        std::string insert_sql =
                            "INSERT INTO videos (id, original_name, hls_path, status, created_at, upload_id) VALUES ('"
                            + escaped_id + "', '" + escaped_name + "', '" + escaped_hls + "', 'uploading', NOW(), '" + escaped_upload_id + "')";
                        
                        if (mysql_query(sql, insert_sql.c_str()) == 0) {
                            if (commitTransaction(sql)) {
                                db_insert_success = true;
                                LOG_INFO("[Transaction] Video record inserted (uploading): %s", video_id.c_str());
                            } else {
                                LOG_ERROR("[Transaction] Commit failed, rolling back: %s", video_id.c_str());
                                rollbackTransaction(sql);
                            }
                        } else {
                            LOG_ERROR("[Transaction] Insert failed: %s", mysql_error(sql));
                            rollbackTransaction(sql);
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("[Transaction] Exception occurred: %s", e.what());
                        if (transaction_started) {
                            rollbackTransaction(sql);
                        }
                    }
                } else {
                    LOG_ERROR("[Transaction] Failed to start transaction");
                }
            } else {
                LOG_ERROR("Failed to get database connection");
            }
        }
        
        // 如果数据库插入失败，直接返回
        if (!db_insert_success) {
            LOG_ERROR("[Step 1 Failed] Database insert failed, aborting upload: %s", video_id.c_str());
            download_in_progress_ = false;
            return false;
        }
        
        // ==================== 步骤 2: 合并分片文件 ====================
        std::ofstream out_file(output_path, std::ios::binary);
        if (!out_file.is_open()) {
            LOG_ERROR("[Step 2 Failed] Failed to create output file: %s", output_path.c_str());
            updateVideoStatus(video_id, false, "");
            download_in_progress_ = false;
            return false;
        }
        
        bool all_chunks_merged = true;
        for (int i = 0; i < total_chunks; ++i) {
            std::string chunk_path = chunk_dir + "/chunk_" + std::to_string(i);
            if (access(chunk_path.c_str(), F_OK) != 0) {
                LOG_ERROR("Chunk file not found: %s", chunk_path.c_str());
                all_chunks_merged = false;
                break;
            }
            std::ifstream chunk_file(chunk_path, std::ios::binary);
            if (chunk_file.is_open()) {
                out_file << chunk_file.rdbuf();
                chunk_file.close();
                std::remove(chunk_path.c_str());
            } else {
                LOG_ERROR("Chunk file open failed: %s", chunk_path.c_str());
                all_chunks_merged = false;
                break;
            }
        }
        out_file.close();
        
        if (!all_chunks_merged) {
            LOG_ERROR("[Step 2 Failed] Chunk merge failed: %s", video_id.c_str());
            std::remove(output_path.c_str());
            updateVideoStatus(video_id, false, "");
            download_in_progress_ = false;
            return false;
        }
        
        LOG_INFO("[Step 2 Success] File merged successfully: %s", output_path.c_str());
        
        // ==================== 步骤 3: 文件写入成功后 UPDATE status='processing' ====================
        {
            SqlConnRAII raii(&sql, SqlConnPool::Instance());
            if (sql) {
                auto escape = [sql](const std::string& s) -> std::string {
                    if (s.empty()) return "";
                    std::string res;
                    res.resize(s.size() * 2 + 1);
                    unsigned long len = mysql_real_escape_string(sql, &res[0], s.c_str(), s.size());
                    res.resize(len);
                    return res;
                };
                
                std::string escaped_id = escape(video_id);
                // 修复：使用条件更新实现乐观锁（只有 status='uploading' 才能更新）
                std::string update_sql = 
                    "UPDATE videos SET status = 'processing' WHERE id = '" + escaped_id + "' AND status = 'uploading'";
                
                if (mysql_query(sql, update_sql.c_str()) == 0) {
                    // 检查是否有行被影响
                    if (mysql_affected_rows(sql) > 0) {
                        LOG_INFO("[Step 3 Success] Video status updated to 'processing': %s", video_id.c_str());
                    } else {
                        LOG_WARN("[Step 3 Warn] No rows affected, status may have changed: %s", video_id.c_str());
                    }
                } else {
                    LOG_ERROR("[Step 3 Failed] Status update failed: %s", mysql_error(sql));
                } 
            }
        }
        
        // ==================== 步骤 4: 启动 HLS 转码（转码完成后 UPDATE status='ready'）====================
        convertToHLSAsync(output_path, hls_path, video_id);
        download_in_progress_ = true;
        
        // 修复：本地锁由 RAII 自动释放
        
        LOG_INFO("[Step 4] HLS conversion started for: %s", video_id.c_str());
    }
    
    return true;
}
bool HttpRequest::ParseRequestLine_(const string& line) {
    // cout<<"ParseRequestLine_ called"<<endl;
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    smatch Match;   // 用来匹配patten得到结果
    // 在匹配规则中，以括号()的方式来划分组别 一共三个括号 [0]表示整体
    if(regex_match(line, Match, patten)) {  // 匹配指定字符串整体是否符合
        method_ = Match[1];
        path_ = Match[2];
        version_ = Match[3];
        state_ = HEADERS;
        // cout<<method_.c_str()<<endl;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

// 解析路径，统一一下path名称,方便后面解析资源
void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html";
    } else {
        if(DEFAULT_HTML.find(path_) != DEFAULT_HTML.end()) {
            path_ += ".html";
        }
    }
}

void HttpRequest::openVideoFile() {
        if (!filename_.empty() && !file_opened_) {
            size_t pos=filename_.find_last_of("/");
            string dir;
            if(pos!=string::npos)
            {
                dir="./sever_videodata/"+filename_.substr(0,pos);
            }
            mkdir(dir.c_str(), 0755); // 创建目录，忽略错误
            string all_pa="./sever_videodata/" + filename_;
            LOG_INFO("Opening file for writing: %s", all_pa.c_str());
            video_file_.open(all_pa, std::ios::binary);
            if (video_file_.is_open()) {
                file_opened_ = true;
                // std::cout << "Started saving to: " <<all_pa << std::endl;
            } else {
                std::cerr << "Failed to create file: " << all_pa << std::endl;
            }
        }
    }
void HttpRequest::ParseHeader_(const std::string& line) {
    size_t pos = line.find(':');
    if (pos != std::string::npos) {
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 2);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        header_[key] = value;
    }

}

void HttpRequest::ParseBody_(const std::string& line) {
    body_ = line;
    ParsePost_();
    state_ = FINISH;    // 状态转换为下一个状态
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

bool HttpRequest::extractFilenameFromDisposition(const std::string& line) {
    // 示例: Content-Disposition: form-data; name="video"; filename="1.mp4"
    size_t pos = line.find("filename=");
    if (pos == std::string::npos) return false;
    
    filename_ = line.substr(pos + 9); // 9 = len("filename=")
    
    // 去除引号
    if (!filename_.empty() && filename_[0] == '"') {
        size_t end_quote = filename_.find_last_of('"');
        if (end_quote != std::string::npos) {
            filename_ = filename_.substr(1, end_quote - 1);
        }
    }
    
    // 安全清理：防止路径穿越攻击
    return !filename_.empty();
}
    
bool HttpRequest::parseMultipartBoundary() {
        auto it = header_.find("content-type");
        if (it == header_.end()) return false;
        
        const std::string& ct = it->second;
        size_t pos = ct.find("boundary=");
        if (pos == std::string::npos) return false;
        
        boundary_ = ct.substr(pos + 9);
        // 去除引号（如果有）
        if (!boundary_.empty() && boundary_[0] == '"') {
            boundary_ = boundary_.substr(1, boundary_.length() - 2);
        }
        
        boundary_marker_ = "--" + boundary_;
        boundary_end_ = "--" + boundary_ + "--";
        return true;
    }
// 16进制转化为10进制
int HttpRequest::ConverHex(char ch) {
    if(ch >= 'A' && ch <= 'F') 
        return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') 
        return ch -'a' + 10;
    return ch;
}

// 处理post请求
void HttpRequest::ParsePost_() {
    if(method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFromUrlencoded_();     // POST请求体示例
        if(DEFAULT_HTML_TAG.count(path_)) { // 如果是登录/注册的path
            int tag = DEFAULT_HTML_TAG.find(path_)->second; 
            LOG_DEBUG("Tag:%d", tag);
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);  // 为1则是登录
                if(UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } 
                else {
                    path_ = "/error.html";
                }
            }
        }
    }   
}

// 从url中解析编码
void HttpRequest::ParseFromUrlencoded_() {
    if(body_.size() == 0) { return; }

    string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;

    for(; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j);
            j = i + 1;
            break;
        case '+':
            body_[i] = ' ';
            break;
        case '%':
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

std::string HttpRequest::getHlsPathById(std::string& video_id) {
    std::string hls_path;

    MYSQL* sql = nullptr;
    {
        SqlConnRAII raii(&sql, SqlConnPool::Instance());
        if (sql) {
                            auto escape = [sql](const std::string& s) -> std::string {
                    if (s.empty()) return "";
                    std::string res;
                    res.resize(s.size() * 2 + 1); // 转义后最长为 2n+1
                    unsigned long len = mysql_real_escape_string(sql, &res[0], s.c_str(), s.size());
                    res.resize(len);
                    return res;
                };
            // 安全转义
            std::string escaped_id = escape(video_id); // 你需要实现 escapeString
            std::string query = "SELECT hls_path FROM videos WHERE id = '" + escaped_id + "' AND status = 'ready'";
            
            if (mysql_query(sql, query.c_str()) == 0) {
                MYSQL_RES* res = mysql_store_result(sql);
                if (res && mysql_num_rows(res) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    if (row[0]) hls_path = row[0];
                }
                mysql_free_result(res);
            }
        }
    }
    size_t last_slash = hls_path.find_last_of('/');
    std::string dir_path = hls_path.substr(0, last_slash);
    hls_path=dir_path+os_path_;
    
    return hls_path; // 若未找到，返回空字符串
}
bool HttpRequest::UserVerify(const string &name, const string &pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII(&sql, SqlConnPool::Instance());
    assert(sql);
    
    bool flag = false;
    char order[256] = { 0 };
    MYSQL_RES *res = nullptr;
    
    if(!isLogin) { flag = true; }
    /* 查询用户及密码 */
    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    LOG_DEBUG("%s", order);

    if(mysql_query(sql, order)) { 
        mysql_free_result(res);
        return false; 
    }
    res = mysql_store_result(sql);

    while(MYSQL_ROW row = mysql_fetch_row(res)) {
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        string password(row[1]);
        /* 注册行为 且 用户名未被使用*/
        if(isLogin) {
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_INFO("pwd error!");
            }
        } 
        else { 
            flag = false; 
            LOG_INFO("user used!");
        }
    }
    mysql_free_result(res);

    /* 注册行为 且 用户名未被使用*/
    if(!isLogin && flag == true) {
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; 
        }
        flag = true;
    }
    // SqlConnPool::Instance()->FreeConn(sql);
    LOG_DEBUG( "UserVerify success!!");
    return flag;
}

std::string HttpRequest::path() const{
    return path_;
}

std::string& HttpRequest::path(){
    return path_;
}
std::string& HttpRequest::re_path(){
    size_t pos = path_.find("vid_");
    if(pos == std::string::npos)
        return path_;

    size_t start = pos;
    size_t end   = path_.find('/', start);
    os_path_=path_.substr(end,path_.size()-end);
    path_ = (end == std::string::npos)
            ? path_.substr(start)
            : path_.substr(start, end - start);
    // cout<<"path"<<path_.c_str()<<endl;
    return path_; 
}

std::string HttpRequest::method() const {
    return method_;
}

std::string HttpRequest::version() const {
    return version_;
}

std::string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

std::string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

bool HttpRequest::IsKeepAlive() const {
    if(header_.count("Connection") == 1) {
        return header_.find("Connection")->second == "keep-alive" && version_ == "1.1";
    }
    return false;
}

// 新增：打开分片文件
void HttpRequest::openChunkFile() {
    if (file_id_.empty() || chunk_index_ < 0) {
        std::cerr << "Invalid file_id or chunk_index" << std::endl;
        return;
    }
    
    std::string chunk_dir = "./sever_videodata/" + file_id_;
    if (system(("mkdir -p " + chunk_dir).c_str()) != 0) {
        LOG_WARN("[Chunk] mkdir failed for: %s", chunk_dir.c_str());
    }
    
    std::string chunk_path = chunk_dir + "/chunk_" + std::to_string(chunk_index_);
    LOG_INFO("Opening chunk file for writing: %s", chunk_path.c_str());
    
    video_file_.open(chunk_path, std::ios::binary);
    if (video_file_.is_open()) {
        file_opened_ = true;
    } else {
        std::cerr << "Failed to create chunk file: " << chunk_path << std::endl;
    }
}

// 修复：校验分片 MD5（分片级别秒传核心）
bool HttpRequest::verifyChunkMD5() {
    if (chunk_md5_.empty()) {
        LOG_WARN("No chunk MD5 provided, skipping verification");
        return true;
    }
    
    std::string chunk_dir = "./sever_videodata/" + file_id_;
    std::string chunk_path = chunk_dir + "/chunk_" + std::to_string(chunk_index_);
    
    std::string local_md5 = MD5::hashFile(chunk_path);
    LOG_INFO("Verifying chunk %d MD5: video_id=%s, client=%s, server=%s", 
             chunk_index_, file_id_.c_str(), chunk_md5_.c_str(), local_md5.c_str());
    
    if (local_md5 != chunk_md5_) {
        LOG_ERROR("Chunk MD5 mismatch! Deleting corrupted chunk.");
        // 删除损坏的分片文件
        std::remove(chunk_path.c_str());
        
        // 修复：清理 Redis 中该分片的记录（防止脏数据）
        clearRedisRecord(file_id_);
        
        // 新增：记录失败日志，包含详细信息
        LOG_ERROR("[MD5 Verify Failed] video_id=%s, chunk=%d, expected=%s, got=%s",
                  file_id_.c_str(), chunk_index_, chunk_md5_.c_str(), local_md5.c_str());
        
        return false;
    }
    
    return true;
}

// 修复：更新 Redis Bitmap（使用 video_id + chunk_index 跟踪分片）
void HttpRequest::updateRedisBitmap() {
    if (file_id_.empty() || chunk_index_ < 0) return;
    
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) {
        LOG_ERROR("Failed to get Redis connection");
        return;
    }
    
    // 修复：使用 upload:{video_id} 作为 key，chunk_index 作为 bit 位置
    std::string key = "upload:" + file_id_;
    redisReply* reply = (redisReply*)redisCommand(redis, "SETBIT %s %d 1", key.c_str(), chunk_index_);
    
    if (reply) {
        LOG_INFO("Redis SETBIT success for %s chunk %d", key.c_str(), chunk_index_);
        freeReplyObject(reply);
    } else {
        LOG_ERROR("Redis SETBIT failed for %s chunk %d", key.c_str(), chunk_index_);
    }
    
    RedisConnPool::Instance()->ReturnConnection(redis);
}

// 修复：检查上传是否完成（基于 Redis Bitmap）
bool HttpRequest::checkUploadComplete(const std::string& file_id, int total_chunks) {
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) {
        LOG_ERROR("Failed to get Redis connection for checkUploadComplete");
        return false;
    }
    
    std::string key = "upload:" + file_id;
    redisReply* reply = (redisReply*)redisCommand(redis, "BITCOUNT %s", key.c_str());
    
    bool complete = false;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        int uploaded_count = reply->integer;
        LOG_INFO("Redis BITCOUNT: %d/%d chunks uploaded", uploaded_count, total_chunks);
        complete = (uploaded_count == total_chunks);
        freeReplyObject(reply);
    }
    
    RedisConnPool::Instance()->ReturnConnection(redis);
    return complete;
}

// 新增：清理 Redis 记录
void HttpRequest::clearRedisRecord(const std::string& file_id) {
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) return;
    
    std::string key = "upload:" + file_id;
    redisReply* reply = (redisReply*)redisCommand(redis, "DEL %s", key.c_str());
    
    if (reply) {
        LOG_INFO("Redis DEL success for %s", key.c_str());
        freeReplyObject(reply);
    }
    
    RedisConnPool::Instance()->ReturnConnection(redis);
}

// 修复：检查分片是否已存在（秒传核心 - 分片级别）
bool HttpRequest::checkChunkExistsByMD5(const std::string& video_id, int chunk_index, const std::string& chunk_md5) {
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) {
        LOG_ERROR("Failed to get Redis connection for chunk MD5 check");
        return false;
    }
    
    // 修复：使用 md5_chunk:{video_id}:{chunk_index} 作为 key 查询分片 MD5
    std::string key = "md5_chunk:" + video_id + ":" + std::to_string(chunk_index);
    redisReply* reply = (redisReply*)redisCommand(redis, "GET %s", key.c_str());
    
    bool exists = false;
    if (reply && reply->type == REDIS_REPLY_STRING && reply->len > 0) {
        std::string existing_md5(reply->str, reply->len);
        if (existing_md5 == chunk_md5) {
            // MD5 匹配，分片已存在且内容一致
            LOG_INFO("[秒传] 分片已存在！video_id=%s, chunk=%d, md5=%s", 
                     video_id.c_str(), chunk_index, chunk_md5.c_str());
            exists = true;
        } else {
            LOG_WARN("[秒传] 分片 MD5 不匹配，需要重新上传：video_id=%s, chunk=%d", 
                     video_id.c_str(), chunk_index);
        }
    }
    
    if (reply) freeReplyObject(reply);
    RedisConnPool::Instance()->ReturnConnection(redis);
    return exists;
}

// 修复：记录分片 MD5 映射（分片上传完成后调用）
void HttpRequest::recordChunkMD5(const std::string& video_id, int chunk_index, const std::string& chunk_md5) {
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) {
        LOG_ERROR("Failed to get Redis connection for chunk MD5 record");
        return;
    }
    
    // 修复：存储 md5_chunk:{video_id}:{chunk_index} -> chunk_md5 映射
    std::string key = "md5_chunk:" + video_id + ":" + std::to_string(chunk_index);
    redisReply* reply = (redisReply*)redisCommand(redis, "SET %s %s", key.c_str(), chunk_md5.c_str());
    
    if (reply) {
        LOG_INFO("记录分片 MD5 映射：%s -> %s", key.c_str(), chunk_md5.c_str());
        freeReplyObject(reply);
    } else {
        LOG_ERROR("记录分片 MD5 映射失败：%s", key.c_str());
    }
    
    RedisConnPool::Instance()->ReturnConnection(redis);
}

// 新增：检查文件上传状态（基于 Redis Bitmap）
bool HttpRequest::checkFileUploadStatus(const std::string& video_id, int total_chunks) {
    redisContext* redis = RedisConnPool::Instance()->GetConnection();
    if (!redis) {
        LOG_ERROR("Failed to get Redis connection for file upload status check");
        return false;
    }
    
    std::string key = "upload:" + video_id;
    redisReply* reply = (redisReply*)redisCommand(redis, "BITCOUNT %s", key.c_str());
    
    bool complete = false;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        int uploaded_count = reply->integer;
        LOG_INFO("文件上传状态检查：%s, %d/%d 分片已上传", 
                 video_id.c_str(), uploaded_count, total_chunks);
        complete = (uploaded_count == total_chunks);
        freeReplyObject(reply);
    }
    
    RedisConnPool::Instance()->ReturnConnection(redis);
    return complete;
}

void HttpRequest::updateVideoStatus(const std::string& video_id, bool success, const std::string& hls_url) {
    MYSQL* sql = nullptr;
    {
        SqlConnRAII raii(&sql, SqlConnPool::Instance());
        if (!sql) {
            LOG_ERROR("Failed to get DB connection for status update: %s", video_id.c_str());
            return;
        }

        // ==================== 使用事务更新状态 ====================
        bool transaction_started = false;
        
        if (beginTransaction(sql)) {
            transaction_started = true;
        }
        
        auto escape = [sql](const std::string& s) -> std::string {
            if (s.empty()) return "";
            std::string res;
            res.resize(s.size() * 2 + 1);
            unsigned long len = mysql_real_escape_string(sql, &res[0], s.c_str(), s.size());
            res.resize(len);
            return res;
        };

        std::string escaped_id = escape(video_id);
        if (success) {
            std::string escaped_url = escape(hls_url);
            // 修复：使用条件更新实现乐观锁（只有 status='processing' 才能更新为 ready）
            std::string sql_str = 
                "UPDATE videos SET hls_path = '" + escaped_url + "', status = 'ready' WHERE id = '" + escaped_id + "' AND status = 'processing'";
            if (mysql_query(sql, sql_str.c_str()) == 0) {
                if (mysql_affected_rows(sql) > 0) {
                    if (transaction_started) {
                        commitTransaction(sql);
                    }
                    LOG_INFO("Video ready: %s -> %s", video_id.c_str(), hls_url.c_str());
                } else {
                    LOG_WARN("Video status update skipped, current status may not be 'processing': %s", video_id.c_str());
                    if (transaction_started) {
                        rollbackTransaction(sql);
                    }
                }
            } else {
                LOG_ERROR("Video update failed: %s", mysql_error(sql));
                if (transaction_started) {
                    rollbackTransaction(sql);
                }
            }
        } else {
            // 修复：失败状态更新不需要条件（任何状态都可以更新为 failed）
            std::string sql_str = 
                "UPDATE videos SET status = 'failed' WHERE id = '" + escaped_id + "'";
            if (mysql_query(sql, sql_str.c_str()) == 0) {
                if (transaction_started) {
                    commitTransaction(sql);
                }
                LOG_INFO("Video failed: %s", video_id.c_str());
            } else {
                LOG_ERROR("Video status update failed: %s", mysql_error(sql));
                if (transaction_started) {
                    rollbackTransaction(sql);
                }
            }
        }
        // ==================== 事务结束 ====================
    }
}

/* ==================== Range Header 实现（断点续传）==================== */

bool HttpRequest::parseRangeHeader() {
    // 从 header_ 中查找 Range 字段
    auto it = header_.find("Range");
    if (it == header_.end()) {
        has_range_ = false;
        return false;
    }
    
    const std::string& range_value = it->second;
    // 解析格式："bytes=start-end" 或 "bytes=start-"
    std::regex range_regex(R"(bytes=(\d+)-(\d*))");
    std::smatch match;
    
    if (std::regex_match(range_value, match, range_regex)) {
        has_range_ = true;
        range_start_ = std::stoull(match[1].str());
        
        if (match[2].matched && !match[2].str().empty()) {
            range_end_ = std::stoull(match[2].str());
        } else {
            range_end_ = 0;  // 0 表示到文件末尾
        }
        LOG_INFO("[Range] Parse success: start=%zu, end=%zu", range_start_, range_end_);
        return true;
    }
    
    has_range_ = false;
    LOG_WARN("[Range] Parse failed: %s", range_value.c_str());
    return false;
}

bool HttpRequest::hasRange() const {
    return has_range_;
}

size_t HttpRequest::rangeStart() const {
    return range_start_;
}

size_t HttpRequest::rangeEnd() const {
    return range_end_;
}

size_t HttpRequest::rangeLength() const {
    if (has_range_ && range_end_ > 0) {
        return range_end_ - range_start_ + 1;
    }
    return 0;  // 未知长度（到文件末尾）
}

/* ==================== MySQL 事务实现（元数据一致性保证）==================== */

bool HttpRequest::beginTransaction(MYSQL* conn) {
    if (!conn) {
        return false;
    }
    
    // 关闭自动提交
    if (mysql_autocommit(conn, 0) != 0) {
        LOG_ERROR("[Transaction] Failed to disable autocommit: %s", mysql_error(conn));
        return false;
    }
    
    // 开始事务
    if (mysql_query(conn, "START TRANSACTION") != 0) {
        LOG_ERROR("[Transaction] Failed to start transaction: %s", mysql_error(conn));
        mysql_autocommit(conn, 1);  // 恢复自动提交
        return false;
    }
    
    LOG_INFO("[Transaction] Transaction started");
    return true;
}

bool HttpRequest::commitTransaction(MYSQL* conn) {
    if (!conn) {
        return false;
    }
    
    if (mysql_query(conn, "COMMIT") != 0) {
        LOG_ERROR("[Transaction] Failed to commit: %s", mysql_error(conn));
        rollbackTransaction(conn);  // 提交失败则回滚
        return false;
    }
    
    // 恢复自动提交
    mysql_autocommit(conn, 1);
    LOG_INFO("[Transaction] Transaction committed");
    return true;
}

bool HttpRequest::rollbackTransaction(MYSQL* conn) {
    if (!conn) {
        return false;
    }
    
    if (mysql_query(conn, "ROLLBACK") != 0) {
        LOG_ERROR("[Transaction] Failed to rollback: %s", mysql_error(conn));
        // 即使回滚失败也要恢复状态
    }
    
    // 恢复自动提交
    mysql_autocommit(conn, 1);
    LOG_INFO("[Transaction] Transaction rolled back");
    return true;
}
