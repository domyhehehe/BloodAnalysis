#include <iostream>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>
#include <unordered_set>


// Windows関係のヘッダをインクルードする前にこれを書く
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif __linux__
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

// ===== RocksDB ヘッダ =====
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

using namespace std;
namespace fs = std::filesystem;

// 未知の父・母
static const string UNKNOWN_SIRE = "UNKNOWN_SIRE";
static const string UNKNOWN_DAM = "UNKNOWN_DAM";


// (オプション) メモリ使用量をモニタリングしたい場合
size_t getMemoryUsageMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(),
        (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);  // MBに変換
    }
    return 0;
#elif __linux__
    // Linuxの場合
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss / 1024; // KB→MB
    }
    return 0;
#else
    return 0;
#endif
}

//--------------------------------------------------------------------
// 馬の情報
//--------------------------------------------------------------------
struct Horse {
    string PrimaryKey;
    string HorseName;
    string YearStr;
    int    YearInt;
    string Sire;
    string Dam;
};

// 全馬データ
unordered_map<string, Horse> horses;
unordered_map<string, string> keyToDisplayName;

//--------------------------------------------------------------------
// CSV行を(カンマ区切り+クォート対応)で分割
//--------------------------------------------------------------------
vector<string> parseCSVLine(const string& line) {
    vector<string> result;
    bool inQuotes = false;
    string value;
    for (char c : line) {
        if (c == '"') {
            inQuotes = !inQuotes;
        }
        else if (c == ',' && !inQuotes) {
            result.push_back(value);
            value.clear();
        }
        else {
            value.push_back(c);
        }
    }
    result.push_back(value);
    return result;
}

//--------------------------------------------------------------------
// 年を int に変換 (失敗時 INT_MIN)
//--------------------------------------------------------------------
int parseYearInt(const string& y) {
    if (y.empty()) return INT_MIN;
    try {
        return stoi(y);
    }
    catch (...) {
        return INT_MIN;
    }
}

//--------------------------------------------------------------------
// CSVから血統データをロード
//--------------------------------------------------------------------
void loadBloodlineCSV(const string& filename) {
    ifstream ifs(filename);
    if (!ifs.is_open()) {
        cerr << "[loadBloodlineCSV] cannot open " << filename << endl;
        exit(1);
    }

    // ヘッダーを読み飛ばす
    string header;
    getline(ifs, header);

    int lineCount = 0;
    while (true) {
        string line;
        if (!getline(ifs, line)) break;
        if (line.empty()) continue;

        vector<string> cols = parseCSVLine(line);
        if (cols.size() < 9) continue; // 不正な行はスキップ

        string pk = cols[0];
        string sire = cols[1];
        string dam = cols[2];
        string ystr = cols[5];
        string name = cols[8];

        if (sire.empty()) sire = UNKNOWN_SIRE;
        if (dam.empty())  dam = UNKNOWN_DAM;

        Horse h;
        h.PrimaryKey = pk;
        h.HorseName = name;
        h.YearStr = ystr;
        h.YearInt = parseYearInt(ystr);
        h.Sire = sire;
        h.Dam = dam;

        horses[pk] = h;
        keyToDisplayName[pk] = name + " [" + ystr + "]";
        lineCount++;
    }
    ifs.close();

    cout << "[loadBloodlineCSV] " << lineCount << "行 読み込み完了, horses.size()="
        << horses.size() << endl;
}

//--------------------------------------------------------------------
// RocksDB の DB インスタンス (グローバル)
rocksdb::DB* g_db = nullptr;

//--------------------------------------------------------------------
// RocksDBで (target, ancestor) をキーとし、double値を保存/取得
//
// Key:   target + "|" + ancestor
// Value: doubleをシリアライズしたバイナリ
//--------------------------------------------------------------------
inline string makeKey(const string& target, const string& ancestor) {
    // シンプルに "target|ancestor" という文字列にする
    return target + "|" + ancestor;
}

inline string serializeDouble(double val) {
    // double(8バイト)をそのままバイナリとして保存
    return string(reinterpret_cast<const char*>(&val), sizeof(double));
}

inline double deserializeDouble(const string& bin) {
    double ret;
    memcpy(&ret, bin.data(), sizeof(double));
    return ret;
}

//--------------------------------------------------------------------
// RocksDBから (target, ancestor) の値を取得
//    見つからなければ optional<double>() を返す (C++17なら optional)
//--------------------------------------------------------------------
inline bool getValueFromDB(const string& target,
    const string& ancestor,
    double& outVal)
{
    string key = makeKey(target, ancestor);
    string val;
    rocksdb::Status s = g_db->Get(rocksdb::ReadOptions(), key, &val);
    if (s.ok()) {
        outVal = deserializeDouble(val);
        return true;
    }
    return false;
}

//--------------------------------------------------------------------
// RocksDBへ (target, ancestor) の値を書き込み
//--------------------------------------------------------------------
inline void putValueToDB(const string& target,
    const string& ancestor,
    double dval)
{
    string key = makeKey(target, ancestor);
    rocksdb::Status s = g_db->Put(rocksdb::WriteOptions(), key, serializeDouble(dval));
    if (!s.ok()) {
        cerr << "[RocksDB] Put error: " << s.ToString() << endl;
    }
}

//--------------------------------------------------------------------
// 血量計算 (RocksDBにキャッシュ)  - dp廃止
//--------------------------------------------------------------------
double getBloodPercentageMemo(const string& target,
    const string& ancestor,
    unordered_set<string>& inStack)
{
    // 1) RocksDBにデータがあれば即リターン
    double cachedVal;
    if (getValueFromDB(target, ancestor, cachedVal)) {
        return cachedVal;
    }

    // 2) horsesにない or UNKNOWN_* => 0
    if (target == UNKNOWN_SIRE || target == UNKNOWN_DAM ||
        !horses.count(target)) {
        putValueToDB(target, ancestor, 0.0);
        return 0.0;
    }

    // 3) 同一 => 1.0
    if (target == ancestor) {
        putValueToDB(target, ancestor, 1.0);
        return 1.0;
    }

    // 4) 循環防止
    if (inStack.count(target)) {
        putValueToDB(target, ancestor, 0.0);
        return 0.0;
    }
    inStack.insert(target);

    // 5) 父・母を辿る再帰
    const Horse& h = horses[target];
    double val = 0.5 * getBloodPercentageMemo(h.Sire, ancestor, inStack)
        + 0.5 * getBloodPercentageMemo(h.Dam, ancestor, inStack);

    inStack.erase(target);

    // 6) RocksDBに保存
    putValueToDB(target, ancestor, val);
    return val;
}

//--------------------------------------------------------------------
// 行列形式CSV出力
//   行=rowKeys, 列=colKeys
//   "HorseName, col1, col2, ..."
//--------------------------------------------------------------------
void saveCSVMatrix_NoExtra(const string& filename,
    const vector<string>& rowKeys,
    const vector<string>& colKeys)
{
    if (colKeys.empty()) {
        cout << "[saveCSVMatrix_NoExtra] skip because colKeys empty\n";
        return;
    }

    ofstream ofs(filename);
    if (!ofs.is_open()) {
        cerr << "[saveCSVMatrix_NoExtra] cannot open " << filename << endl;
        return;
    }
    ofs << fixed << setprecision(8);

    // ヘッダ
    ofs << "HorseName";
    for (auto& ck : colKeys) {
        ofs << "," << keyToDisplayName[ck];
    }
    ofs << "\n";

    // 本文
    for (size_t i = 0; i < rowKeys.size(); i++) {
        const string& rk = rowKeys[i];
        size_t memUsage = getMemoryUsageMB();

        cout << "[計算中] " << (i + 1) << "/" << rowKeys.size()
            << ": " << keyToDisplayName[rk]
            << " (Mem=" << memUsage << "MB)"
                << endl;

            ofs << keyToDisplayName[rk];

            // 列ループ
            for (auto& ck : colKeys) {
                unordered_set<string> inStack;
                double val = getBloodPercentageMemo(rk, ck, inStack);
                if (fabs(val) < 1e-12) val = 0.0;
                ofs << "," << val;
            }
            ofs << "\n";
    }
    ofs.close();

    cout << "[saveCSVMatrix_NoExtra] " << filename << " 出力完了\n";
}

//--------------------------------------------------------------------
// メイン (RocksDBを使用し、dp廃止バージョン)
//--------------------------------------------------------------------
int main()
{


    std::cout << "現在の実行ディレクトリ: " << std::filesystem::current_path() << std::endl;

    try {
        // 1) 血統データCSV読み込み
        loadBloodlineCSV("C:\\Users\\user\\source\\repos\\BloodAnalysis\\BloodAnalysis\\BloodAnalysis\\bloodline.csv");


        // 2) RocksDBオープン
        {
            rocksdb::Options options;
            options.create_if_missing = true;
            rocksdb::Status s = rocksdb::DB::Open(options, "blood_cache_db", &g_db);
            if (!s.ok()) {
                cerr << "[RocksDB] Open error: " << s.ToString() << endl;
                return 1;
            }
        }

        // 3) 全馬キーをソート
        vector<string> allKeys;
        allKeys.reserve(horses.size());
        for (auto& kv : horses) {
            allKeys.push_back(kv.first);
        }
        sort(allKeys.begin(), allKeys.end(), [&](auto& lhs, auto& rhs) {
            int yl = horses[lhs].YearInt;
            int yr = horses[rhs].YearInt;
            if (yl == yr) return lhs < rhs;
            return yl < yr;
            });

        cout << "[main] allKeys.size()=" << allKeys.size() << endl;

        // 4) 年代区分に分割 (例)
        vector<string> subset_0_1800;
        vector<string> subset_1801_1900;
        vector<string> subset_1901_2000;
        vector<string> subset_2001;

        for (auto& k : allKeys) {
            int y = horses[k].YearInt;
            if (y == INT_MIN || y <= 1800) {
                subset_0_1800.push_back(k);
            }
            else if (y <= 1900) {
                subset_1801_1900.push_back(k);
            }
            else if (y <= 2000) {
                subset_1901_2000.push_back(k);
            }
            else {
                subset_2001.push_back(k);
            }
        }

        // 5) 行列形式でCSV出力
        saveCSVMatrix_NoExtra("blood_percentage_0_1800.csv", allKeys, subset_0_1800);
        saveCSVMatrix_NoExtra("blood_percentage_1801_1900.csv", allKeys, subset_1801_1900);
        saveCSVMatrix_NoExtra("blood_percentage_1901_2000.csv", allKeys, subset_1901_2000);
        saveCSVMatrix_NoExtra("blood_percentage_2001.csv", allKeys, subset_2001);

        // 6) RocksDBをクローズ
        delete g_db;
        g_db = nullptr;

        cout << "[main] 全処理完了\n";
    }
    catch (const exception& e) {
        cerr << "例外発生: " << e.what() << endl;
        return 1;
    }
    return 0;
}
