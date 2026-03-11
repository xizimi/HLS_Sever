#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex>
#include <errno.h>     
#include <mysql.h>
#include <fstream> 
#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"

class HttpRequest {
public:
    enum PARSE_STATE {
        REQUEST_LINE,
        HEADERS,
        BODY,
        BODY_START,
        BODY_DATA,
        BODY_END,
        FINISH,
        ERROR
    };
    
    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    void Init();
    bool parse(Buffer& buff);   
    bool my_parse(Buffer& buff);

    std::string path() const;
    std::string& path();
    std::string method() const;
    std::string version() const;
    std::string GetPost(const std::string& key) const;
    std::string GetPost(const char* key) const;
    bool parseMultipartBoundary();

    bool IsKeepAlive() const;
    bool extractFilenameFromDisposition(const std::string& line);
    void openVideoFile();
    std::string& re_path();
    std::string getHlsPathById(std::string& video_id);

private:
    bool ParseRequestLine_(const std::string& line);
    void ParseHeader_(const std::string& line);
    void ParseBody_(const std::string& line);
    void ParsePath_();
    void ParsePost_();
    void ParseFromUrlencoded_();
    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);
    void convertToHLSAsync(std::string input, std::string outputDir);
    
    // 新增：分片上传相关方法
    void openChunkFile();
    bool verifyChunkMD5();
    void updateRedisBitmap();
    bool checkUploadComplete(const std::string& file_id, int total_chunks);
    void clearRedisRecord(const std::string& file_id);
    static void updateVideoStatus(const std::string& video_id, bool success, const std::string& hls_url);
    
    // 新增：秒传相关方法
    bool checkFileExistsByMD5(const std::string& file_md5);
    void recordFileMD5(const std::string& file_md5, const std::string& file_id);

    PARSE_STATE state_;
    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_;

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    static int ConverHex(char ch);
    
    size_t content_length_ = 0;
    std::ofstream upload_file_;
    std::string upload_filename_;
    std::string boundary_;
    std::string boundary_marker_;
    std::string boundary_end_;
    bool in_file_part_ = false;
    size_t body_received_ = 0;
    std::ofstream video_file_; 
    std::string filename_; 
    bool file_opened_ = false;
    bool is_file_part_ = false;
    std::string SafePath(const std::string& s);
    bool download_in_progress_ = false;
    std::string os_path_ = "";
    bool complete_signal_ = false;
    
    // 分片上传相关字段
    std::string file_id_;
    int chunk_index_ = -1;
    int total_chunks_ = -1;
    std::string chunk_md5_;
};

#endif