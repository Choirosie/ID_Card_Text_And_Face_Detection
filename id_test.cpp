#include <opencv2/opencv.hpp>
#include <mysql.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <json/json.h>
#include <windows.h>
#include "config.h"  // getenv() 헤더

using namespace cv;
using namespace std;
namespace fs = filesystem;

// Base64 인코딩을 위한 함수
string base64_encode(const string& in) {
    static const string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 서버 응답 데이터를 받을 버퍼
size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* s) {
    s->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// 이미지 파일을 Base64로 인코딩하는 함수
string encode_image_to_base64(const string& image_path) {
    ifstream image_file(image_path, ios::binary);
    if (!image_file) {
        cerr << "Error: Could not open file " << image_path << endl;
        return "";
    }
    ostringstream image_data;
    image_data << image_file.rdbuf();
    return base64_encode(image_data.str());
}

// 서버 응답을 처리하는 함수 (Google Vision API 결과 파싱)
string handleResponse(const string& response_string) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    istringstream s(response_string);
    string errs;

    if (Json::parseFromStream(readerBuilder, s, &root, &errs)) {
        const Json::Value& textAnnotations = root["responses"][0]["textAnnotations"];
        if (textAnnotations.isArray() && !textAnnotations.empty()) {
            return textAnnotations[0]["description"].asString();
        }
    }
    return "";
}

// OCR 요청 및 텍스트 추출 함수
string performOCR(const string& api_key, const string& base64_image) {
    CURL* curl;
    CURLcode res;
    string response_string;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        string url = "https://vision.googleapis.com/v1/images:annotate?key=" + api_key;

        string json_data = R"({
            "requests": [
                {
                    "image": {
                        "content": ")" + base64_image + R"("
                    },
                    "features": [
                        {
                            "type": "TEXT_DETECTION"
                        }
                    ]
                }
            ]
        })";

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_ENCODING, "utf-8");

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << endl;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return handleResponse(response_string);
}


string lowercase(const string& s) {
    string result = s;
    transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

//issurer 분리
void splitDateAndIssuer(const std::string& date_issuer, std::string& date, std::string& issuer) {
    // 날짜는 고정된 길이의 10자 (YYYY.MM.DD 형식)
    date = date_issuer.substr(0, 10); // 첫 10자를 날짜로 추출
    issuer = date_issuer.substr(11);   // 그 이후는 발급인으로 추출
}

// 텍스트를 데이터베이스 필드로 분리하는 함수
void saveToDatabase(const vector<string>& data_fields, const string& image_path) {
    MYSQL* conn, connection;
    MYSQL_RES* result;
    MYSQL_ROW row;

    const char* DB_HOST_C = DB_HOST.c_str(); // 호스트명 
    const char* DB_USER_C = DB_USER.c_str(); // 사용자명 
    const char* DB_PASS_C = DB_PASSWORD.c_str(); // config.h에서 정의한 비밀번호 상수를 사용
    char DB_NAME[] = "idcard"; // 데이터베이스명 
    char sql[1024];

    // MySQL 초기화 및 연결
    mysql_init(&connection);
    conn = mysql_real_connect(&connection, DB_HOST_C, DB_USER_C, DB_PASS_C, DB_NAME, 3306, NULL, 0);

    // 필드 값들을 테이블에 맞춰 저장 (필드 개수가 적을 경우 기본 값 적용)
    vector<string> fields = data_fields;
    while (fields.size() < 20) {  // 필요한 필드의 개수에 맞게 필드를 채움
        fields.push_back("NULL");  // 부족한 필드를 NULL로 채움
    }

    // MySQL 이스케이프 처리를 위한 연결 확인
    if (!conn) {
        cerr << "MySQL 연결 실패: " << mysql_error(&connection) << endl;
        return;
    }

    // 각 필드를 MySQL에 맞게 이스케이프 처리
    for (auto& field : fields) {
        char escaped_string[1024];  // 이스케이프된 문자열을 저장할 공간
        mysql_real_escape_string(conn, escaped_string, field.c_str(), field.length());
        field = string(escaped_string);  // 이스케이프된 문자열로 필드 값 교체
    }

    // 필드 개수에 맞춰 삽입 쿼리 실행
    if ((lowercase(fields[0]).find('1') != string::npos) || (lowercase(fields[0]).find('2') != string::npos)) {
        string date, issuer;
        splitDateAndIssuer(fields[11], date, issuer);  // 날짜와 발급인 분리
        string query = "INSERT INTO drivecard (dtype, dltype, dnumber, name, pnumber, address, ddate, dissuer, dimage) "
            "VALUES ('" + fields[0] + "', '" + fields[1] + "', '" + fields[2] + "', '" +
            fields[3] + "', '" + fields[4] + "', '" + fields[5] + fields[6] + "', '" + date + "', '" + issuer + "', '" + image_path + "')";
        strcpy(sql, query.c_str());
    }
    else {
        string query = "INSERT INTO idcard (idtype, name, pnumber, address, pdate, issuer, pimage) "
            "VALUES ('" + fields[0] + "', '" + fields[1] + "', '" + fields[2] + "', '" +
            fields[3] + fields[4] + "', '" + fields[5] + "', '" + fields[6] + "', '" + image_path + "')";
        strcpy(sql, query.c_str());
    }

    if (mysql_query(conn, sql) != 0) {
        cerr << "데이터 추가 오류: " << mysql_error(conn) << endl;
    }
    else {
        cout << "데이터가 성공적으로 저장되었습니다!" << endl;
    }

    mysql_close(conn);  // MySQL 연결 종료
}

// OCR로 추출한 텍스트를 필드로 분리하는 함수 (예: 공백 기준으로 분리)
vector<string> extractFields(const string& extracted_text) {
    vector<string> fields;
    istringstream iss(extracted_text);
    string token;

    while (getline(iss, token, '\n')) {  // 줄 단위로 필드를 분리
        fields.push_back(token);
    }

    return fields;
}
void detectAndSaveFace(const string& image_path, const vector<string>& data_fields) {
    static CascadeClassifier face_cascade;

    if (face_cascade.empty()) {
        if (!face_cascade.load("C:\\OpenCV\\build\\etc\\haarcascades\\haarcascade_frontalface_default.xml")) {
            cerr << "Error: Could not load face cascade classifier." << endl;
            return;
        }
    }

    Mat image = imread(image_path);
    vector<Rect> faces;

    if (image.empty()) {
        cerr << "Error: Could not read image: " << image_path << endl;
        return;
    }

    try {
        face_cascade.detectMultiScale(image, faces, 1.05, 20, 0, Size(50, 50));
    }
    catch (const cv::Exception& e) {
        cerr << "Error during face detection: " << e.what() << endl;
        return;
    }

    for (size_t i = 0; i < faces.size(); i++) {
        Mat face = image(faces[i]);
        string face_image_path = "face_" + to_string(i) + "_" + to_string(time(0)) + ".jpg";

        if (!imwrite(face_image_path, face)) {
            cerr << "Error: Could not save face image: " << face_image_path << endl;
        }
        else {
            cout << "인식된 얼굴이 저장되었습니다: " << face_image_path << endl;

            // 얼굴 이미지 경로를 데이터베이스에 저장
            saveToDatabase(data_fields, face_image_path);
        }
    }

    if (faces.empty()) {
        cerr << "얼굴이 인식되지 않았습니다." << endl;
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8); // UTF-8 코드 페이지 설정
    SetConsoleCP(CP_UTF8);        // 입력 코드 페이지 설정
    string api_key = GOOGLE_API_KEY; // Google Vision API Key
    VideoCapture cap(0);  // 기본 카메라 열기

    if (!cap.isOpened()) {
        cerr << "카메라를 열 수 없습니다." << endl;
        return 1;
    }

    while (true) {
        Mat frame;
        cap >> frame;  // 카메라로부터 프레임 읽기

        if (frame.empty()) {
            cerr << "프레임이 비어 있습니다." << endl;
            break;
        }

        imshow("Camera", frame);

        int key = waitKey(1);
        if (key == 27) {  // ESC 키를 누르면 종료
            break;
        }
        else if (key == 's' || key == 'S') {  // 's' 키를 누르면 스냅샷 저장
            string image_path = "snapshot.jpg";
            imwrite(image_path, frame);  // 이미지 저장

            // Base64로 인코딩 및 OCR 수행
            string base64_image = encode_image_to_base64(image_path);
            string extracted_text = performOCR(api_key, base64_image);

            if (!extracted_text.empty()) {
                cout << extracted_text << endl;

                // 텍스트를 필드로 분리하고 데이터베이스에 저장
                vector<string> data_fields = extractFields(extracted_text);

                // 얼굴 인식 및 저장
                detectAndSaveFace(image_path, data_fields);
            }
            else {
                cerr << "텍스트 인식에 실패했습니다." << endl;
            }
        }
    }

    cap.release();
    destroyAllWindows();

    return 0;
}


