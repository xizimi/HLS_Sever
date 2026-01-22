#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex>    // 正则表达式
#include <errno.h>     
#include <mysql.h>  //mysql
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
    bool ParseRequestLine_(const std::string& line);    // 处理请求行
    void ParseHeader_(const std::string& line);         // 处理请求头
    void ParseBody_(const std::string& line);           // 处理请求体

    void ParsePath_();                                  // 处理请求路径
    void ParsePost_();                                  // 处理Post事件
    void ParseFromUrlencoded_();                        // 从url种解析编码

    

    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);  // 用户验证

    PARSE_STATE state_;
    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_;

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;
    static int ConverHex(char ch);  // 16进制转换为10进制
    size_t content_length_ = 0;   // 新增
    // std::string body_="";            // 新增：存储原始 body 字节
    std::ofstream upload_file_;   //  用于写入上传文件
    std::string upload_filename_; //  临时文件路径
    std::string boundary_;        // multipart/form-data 的 boundary
    std::string boundary_marker_; // 边界标记   
    std::string boundary_end_;    // 结束边界标记
    bool in_file_part_ = false;
    size_t body_received_ = 0;
    std::ofstream video_file_; 
    std::string filename_; 
    bool file_opened_ = false;
    bool is_file_part_ = false;
    std::string SafePath(const std::string& s);
    void convertToHLSAsync(std::string input, std::string outputDir);
    bool download_in_progress_ = false;
    std::string os_path_="";
    bool comlete_singal=false;
};

#endif