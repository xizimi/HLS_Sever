#include "httpconn.h"
using namespace std;

const char* HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

HttpConn::HttpConn() { 
    fd_ = -1;
    addr_ = { 0 };
    isClose_ = true;
};

HttpConn::~HttpConn() { 
    Close(); 
};

void HttpConn::init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    userCount++;
    addr_ = addr;
    fd_ = fd;
    writeBuff_.RetrieveAll();
    readBuff_.RetrieveAll();
    isClose_ = false;
    request_.Init();
    LOG_INFO("Client[%d](%s:%d) in, userCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
}

void HttpConn::Close() {
    response_.UnmapFile();
    if(isClose_ == false){
        isClose_ = true; 
        userCount--;
        close(fd_);
        LOG_INFO("Client[%d](%s:%d) quit, UserCount:%d", fd_, GetIP(), GetPort(), (int)userCount);
    }
}

int HttpConn::GetFd() const {
    return fd_;
};

struct sockaddr_in HttpConn::GetAddr() const {
    return addr_;
}

const char* HttpConn::GetIP() const {
    return inet_ntoa(addr_.sin_addr);
}

int HttpConn::GetPort() const {
    return addr_.sin_port;
}

ssize_t HttpConn::read(int* saveErrno) {

    ssize_t len = -1;
    // std::cout<<"isEt"<<isET<<std::endl;
    do {
        //len = readBuff_.ReadFd(fd_, saveErrno);
        len = readBuff_.ReadFd_my(fd_, saveErrno);
        if (len <= 0) {
            break;
        }
    } while (isET); // ET:边沿触发要一次性全部读出
    /////自己写的
    // readBuff_.RetrieveAll();
    return len;
}

// 主要采用writev连续写函数
ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iovCnt_);   // 将iov的内容写到fd中
        if(len <= 0) {
            *saveErrno = errno;
            break;
        }
        if(iov_[0].iov_len + iov_[1].iov_len  == 0) { break; } /* 传输结束 */
        else if(static_cast<size_t>(len) > iov_[0].iov_len) {
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            if(iov_[0].iov_len) {
                writeBuff_.RetrieveAll();
                iov_[0].iov_len = 0;
            }
        }
        else {
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; 
            iov_[0].iov_len -= len; 
            writeBuff_.Retrieve(len);
        }
    } while(isET || ToWriteBytes() > 10240);
    return len;
}

bool HttpConn::process() {
    request_.Init();
    if(readBuff_.ReadableBytes() <= 0) {
        return false;
    }
    else if(request_.parse(readBuff_)) {    // 解析成功
        LOG_DEBUG("%s", request_.path().c_str());
        response_.Init(srcDir, request_.path(), request_.IsKeepAlive(), 200);
    } else {
        response_.Init(srcDir, request_.path(), false, 400);
    }

    response_.MakeResponse(writeBuff_); // 生成响应报文放入writeBuff_中
    // 响应头
    iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
    iov_[0].iov_len = writeBuff_.ReadableBytes();
    iovCnt_ = 1;

    // 文件
    if(response_.FileLen() > 0  && response_.File()) {
        iov_[1].iov_base = response_.File();
        iov_[1].iov_len = response_.FileLen();
        iovCnt_ = 2;
    }
    LOG_DEBUG("filesize:%d, %d  to %d", response_.FileLen() , iovCnt_, ToWriteBytes());
    return true;
}

bool HttpConn::my_process(int len) {
    // if(readBuff_.ReadableBytes() <= 0) {
    //     return false;
    // }
    if(request_.my_parse(readBuff_)) 
    {
        readBuff_.RetrieveAll();
        return true;
    }else{
        string str=request_.re_path();
        // cout<<"str in httpconn:"<<str<<endl;
        response_.Init(srcDir, str, request_.IsKeepAlive(), 200);
        string data_path=request_.getHlsPathById(str);
        // cout<<"data_path in httpconn:"<<data_path<<endl;
        // cout<<"data_path in httpconn:"<<data_path<<endl;
        // if (data_path.find("m3u8") != std::string::npos)
        // {
        //     size_t last_slash = data_path.find_last_of('/');
        //     os_path_ = data_path.substr(0, last_slash);
        // }
        // if (data_path.find(".ts") != std::string::npos)
        // {
        //     data_path=os_path_+data_path;
        // }
        // cout<<"final data_path in httpconn2:"<<data_path<<endl;
        response_.MakeResponse_my(writeBuff_, data_path);
        iov_[0].iov_base = const_cast<char*>(writeBuff_.Peek());
        iov_[0].iov_len = writeBuff_.ReadableBytes();
        iovCnt_ = 1;
        readBuff_.RetrieveAll();
        return false;
    }
    // readBuff_.RetrieveAll();  
    
    return true;
}
