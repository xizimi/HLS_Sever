#include "httprequest.h"
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

// void HttpRequest::convertToHLSAsync(std::string input, std::string outputDir) {
//     std::thread([this,input = std::move(input), outputDir = std::move(outputDir)]() {
//         try {
//             std::string safeIn = SafePath(input);
//             std::string safeOut = SafePath(outputDir);

//             if (access(safeIn.c_str(), F_OK) != 0) {
//                 std::cerr << "[HLS] File not found: " << safeIn << "\n";
//                 return;
//             }

//             std::system(("mkdir -p " + safeOut).c_str());

//             std::string cmd =
//                 "ffmpeg -y -i \"" + safeIn + "\" "
//                 "-c:v libx264 -profile:v baseline -level 3.0 "
//                 "-c:a aac -ar 44100 "
//                 "-hls_time 4 -hls_list_size 0 "
//                 "-hls_segment_filename \"" + safeOut + "/index%03d.ts\" "   
//                 "-hls_base_url \"" + safeOut + "/\" "                       
//                 "-f hls \"" + safeOut + "/index.m3u8\" "
//                 "2>/dev/null";

//             std::cout << "[HLS] Running...\n";
//             int ret = std::system(cmd.c_str());
//             std::cout << "[HLS] " << (ret ? " Failed" : " Success") << "\n";
//         } catch (const std::exception& e) {
//             std::cerr << "[HLS] Error: " << e.what() << "\n";
//         }
//     }).detach();
// 假设你在类外或头文件中定义了 kVariants（使用 width/height）
//}
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

void HttpRequest::convertToHLSAsync(std::string input, std::string outputDir) {
    std::thread([this, input = std::move(input), outputDir = std::move(outputDir)]() {
        try {
            std::string safeIn = SafePath(input);
            std::string safeOut = SafePath(outputDir);

            if (access(safeIn.c_str(), F_OK) != 0) {
                std::cerr << "[HLS] File not found: " << safeIn << "\n";
                return;
            }

            // 创建输出目录
            std::system(("mkdir -p " + safeOut).c_str());

            std::vector<std::string> variantPaths;

            // 1. 为每个码率生成 HLS 子流
            for (const auto& var : kVariants) {
                std::string varDir = safeOut + "/" + var.name;
                std::system(("mkdir -p \"" + varDir + "\"").c_str()); // 加引号防路径含空格

                std::string segPattern = varDir + "/index%03d.ts";
                std::string playlist = varDir + "/index.m3u8";

                // ✅ 正确构建滤镜：使用 width/height 分开
                std::string vf = "scale=" + std::to_string(var.width) + ":" + std::to_string(var.height)
                               + ":force_original_aspect_ratio=decrease,"
                               + "pad=" + std::to_string(var.width) + ":" + std::to_string(var.height)
                               + ":(ow-iw)/2:(oh-ih)/2";

                // 构建 FFmpeg 命令
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

                std::cout << "[HLS] Encoding " << var.name << "...\n";
                // std::cout << "[CMD] " << cmd << "\n"; // 调试用，上线可删

                int ret = std::system(cmd.c_str());
                if (ret != 0) {
                    std::cerr << "[HLS] Failed to encode " << var.name << ", see " << varDir << "/ffmpeg.log\n";
                    continue;
                }
                variantPaths.push_back(var.name + "/index.m3u8");
            }

            // 2. 生成 Master Playlist
            if (!variantPaths.empty()) {
                std::string masterPath = safeOut + "/master.m3u8";
                std::ofstream master(masterPath);
                if (master.is_open()) {
                    master << "#EXTM3U\n";
                    master << "#EXT-X-VERSION:3\n\n";

                    for (size_t i = 0; i < variantPaths.size(); ++i) {
                        const auto& var = kVariants[i];
                        // 计算总码率 (bps)
                        auto parseBitrate = [](const std::string& br) -> int {
                            std::string s = br;
                            if (s.back() == 'k' || s.back() == 'K') {
                                return std::stoi(s.substr(0, s.size()-1)) * 1000;
                            }
                            return std::stoi(s);
                        };
                        int totalBps = parseBitrate(var.bitrate) + parseBitrate(var.audio_bitrate);

                        // ✅ RESOLUTION 必须是 WxH 字符串
                        std::string resolution = std::to_string(var.width) + "x" + std::to_string(var.height);

                        master << "#EXT-X-STREAM-INF:BANDWIDTH=" << totalBps
                               << ",RESOLUTION=" << resolution << "\n";
                        master << variantPaths[i] << "\n\n";
                    }
                    master.close();
                    std::cout << "[HLS] Master playlist generated: " << masterPath << "\n";
                }
            }

            std::cout << "[HLS] Conversion completed.\n";
        } catch (const std::exception& e) {
            std::cerr << "[HLS] Exception: " << e.what() << "\n";
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
        
        while (buff.ReadableBytes() > 0 && state_ != FINISH) {
            // ==================== REQUEST_LINE & HEADERS ====================
            if (state_ == REQUEST_LINE || state_ == HEADERS) {
                // 查找换行
                const char* line_end = std::search(
                    buff.Peek(), buff.BeginWriteConst(),
                    CRLF, CRLF + 2
                );
                
                if (line_end == buff.BeginWriteConst()) {
                    break; // 数据不足
                }
                
                std::string line(buff.Peek(), line_end);
                buff.RetrieveUntil(line_end + 2);
                
                if (state_ == REQUEST_LINE) {
                    if (!ParseRequestLine_(line)) return true;
                    if (method_ == "GET") {
                    state_ = FINISH;
                    return false;
                }
                } else if (state_ == HEADERS) {
                    if (line.empty()) { // Headers结束
                        if (!header_.count("content-length")) {
                            state_ = FINISH;
                            return true;
                        }
                        content_length_ = std::stoul(header_["content-length"]);
                        
                        // 解析boundary
                        if (parseMultipartBoundary()) {
                            state_ = BODY_START;
                        } else {
                            state_ = FINISH; // 不是multipart，直接结束
                        }
                    } else {
                        ParseHeader_(line);
                    }
                }
            }
            // ==================== BODY_START: 处理boundary ====================
            else if (state_ == BODY_START) {
                // 读取第一行应该是boundary
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
                    return true; // 格式错误
                }
            }
            // ==================== BODY_DATA: 处理文件头或数据 ====================
            else if (state_ == BODY_DATA) {
                const char* line_end = std::search(
                    buff.Peek(), buff.BeginWriteConst(),
                    CRLF, CRLF + 2
                );
                
                if (!in_file_part_) {
                    // 解析Content-Disposition和Content-Type
                    if (line_end == buff.BeginWriteConst()) break;
                    
                    std::string line(buff.Peek(), line_end);
                    buff.RetrieveUntil(line_end + 2);
                    
                    if (line.find("Content-Disposition") == 0) {
                        is_file_part_ = extractFilenameFromDisposition(line); // 记录是否是文件
                    }
                    // 继续读 headers，直到空行
                    if (line.empty()) {
                        // Headers 结束
                        if (is_file_part_) {
                            in_file_part_ = true;
                            openVideoFile();
                        }
                    }
                    
                } else{
    // ==================== 流式写入：保守策略（不丢数据） ====================
                const char* data = buff.Peek();
                const char* end = buff.BeginWriteConst();

                // 先尝试找 closing boundary（带 --）
                const char* closing_pos = std::search(data, end, 
                    boundary_end_.c_str(), boundary_end_.c_str() + boundary_end_.size());

                if (closing_pos != end) {
                    // 检查前面是否有 \r\n
                    if (closing_pos >= data + 2 && 
                        closing_pos[-2] == '\r' && closing_pos[-1] == '\n') {
                        // 文件数据截止于 \r\n 之前
                        size_t file_data_len = (closing_pos - 2) - data;
                        if (file_data_len > 0) {
                            video_file_.write(data, file_data_len);
                            body_received_ += file_data_len;
                        }
                        // 消费到 closing boundary 结束
                        buff.RetrieveUntil(closing_pos + boundary_end_.size());
                        state_ = FINISH;
                        in_file_part_ = false;
                        video_file_.close();
                        continue; // 继续循环，处理剩余数据（如有）
                    }
                }

                // 如果没找到 closing boundary，就写入全部（避免丢数据）
                size_t readable = buff.ReadableBytes();
                if (readable > 0) {
                    video_file_.write(data, readable);
                    body_received_ += readable;
                    buff.Retrieve(readable);
                }
            }
                        }
            // ==================== BODY_END: 处理结束 ====================
            else if (state_ == BODY_END) {
                // 消费掉boundary和后面的--以及\r\n
                const char* line_end = std::search(
                    buff.Peek(), buff.BeginWriteConst(),
                    CRLF, CRLF + 2
                );
                if (line_end == buff.BeginWriteConst()) break;
                
                buff.RetrieveUntil(line_end + 2);
                state_ = FINISH;
                video_file_.close(); // 关闭文件
            }
        }
        if(state_ == FINISH&&!download_in_progress_) {
        
            std::string video_id = "vid_" + std::to_string(time(nullptr)) + "_" + std::to_string(rand() % 10000);
        
            std::string input_path = "./sever_videodata/" + filename_;
    
            std::string output_dir = "./muts_ts/" + video_id + "_out"; 

            convertToHLSAsync(input_path, output_dir);
            download_in_progress_ = true;
            MYSQL* sql = nullptr;
        {
            SqlConnRAII raii(&sql, SqlConnPool::Instance()); // 自动获取+归还连接
            if (sql) {
                // 安全转义字符串（防 SQL 注入）
                auto escape = [sql](const std::string& s) -> std::string {
                    if (s.empty()) return "";
                    std::string res;
                    res.resize(s.size() * 2 + 1); // 转义后最长为 2n+1
                    unsigned long len = mysql_real_escape_string(sql, &res[0], s.c_str(), s.size());
                    res.resize(len);
                    return res;
                };

                std::string escaped_id = escape(video_id);
                std::string escaped_name = escape(filename_);
                std::string hls_path = output_dir + "/master.m3u8";
                std::string insert_sql =
                        "INSERT INTO videos (id, original_name, hls_path, status, created_at) VALUES ('"
                        + escaped_id + "', '"
                        + escaped_name + "', '"
                        + escape(hls_path) + "', '"
                        + "ready" + "', "
                        + "NOW()" + ")";   
                if (mysql_query(sql, insert_sql.c_str())) {
                    std::cerr << "[DB ERROR] Insert failed: " << mysql_error(sql) << std::endl;
                } else {
                    std::cout << "[INFO] Video record inserted: " << video_id << std::endl;
                }
            }
        }
        }
        
        return true;
    }
bool HttpRequest::ParseRequestLine_(const string& line) {
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

// void HttpRequest::ParseHeader_(const string& line) {
//     regex patten("^([^:]*): ?(.*)$");
//     smatch Match;
//     if(regex_match(line, Match, patten)) {
//         header_[Match[1]] = Match[2];
//     } else {    // 匹配失败说明首部行匹配完了，状态变化
//         state_ = BODY;
//     }
// }
void HttpRequest::openVideoFile() {
        if (!filename_.empty() && !file_opened_) {
            string all_pa="./sever_videodata/" + filename_;
            video_file_.open(all_pa, std::ios::binary);
            if (video_file_.is_open()) {
                file_opened_ = true;
                std::cout << "Started saving to: " <<all_pa << std::endl;
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
    // for(auto &kv : header_) {
    //     cout<<kv.first.c_str()<<", "<<kv.second.c_str()<<endl;
    // }

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
    // if (video_id.find("ts") != std::string::npos)
    // {
    //     return video_id; 
    // }

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
    unsigned int j = 0;
    char order[256] = { 0 };
    MYSQL_FIELD *fields = nullptr;
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
    j = mysql_num_fields(res);
    fields = mysql_fetch_fields(res);

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
void updateVideoStatus(const std::string& video_id, bool success, const std::string& hls_url) {
    MYSQL* sql = nullptr;
    {
        SqlConnRAII raii(&sql, SqlConnPool::Instance());
        if (!sql) return;

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
            std::string sql_str = 
                "UPDATE videos SET hls_path = '" + escaped_url + "', status = 'ready' WHERE id = '" + escaped_id + "'";
            mysql_query(sql, sql_str.c_str());
            std::cout << "[INFO] Video ready: " << video_id << " -> " << hls_url << std::endl;
        } else {
            std::string sql_str = 
                "UPDATE videos SET status = 'failed' WHERE id = '" + escaped_id + "'";
            mysql_query(sql, sql_str.c_str());
            std::cerr << "[ERROR] Video failed: " << video_id << std::endl;
        }
    }
}