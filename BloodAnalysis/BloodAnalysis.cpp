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

//--------------------------------------------------------------------
// メモリ使用量を取得 (任意)
//--------------------------------------------------------------------
size_t getMemoryUsageMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(),
        (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);  // MBに変換
    }
    return 0;
#elif __linux__
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
unordered_map<string, Horse> horses;            // pk => Horse構造体
unordered_map<string, string> keyToDisplayName; // pk => "馬名 [年]"

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
    return target + "|" + ancestor;  // "target|ancestor"
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
//    成功なら outVal に代入し true
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
// 血量計算 (RocksDBにキャッシュ) - 通常版 (target→ancestor)
//   - すでにあるならDBから読み取り
//   - なければ計算してDBに書き込み
//--------------------------------------------------------------------
double getBloodPercentageMemo_normal(const string& target,
    const string& ancestor,
    unordered_set<string>& inStack)
{
    // 1) RocksDBにキャッシュあり？
    double cachedVal;
    if (getValueFromDB(target, ancestor, cachedVal)) {
        return cachedVal;
    }

    // 2) horsesにない or UNKNOWN => 0
    if (target == UNKNOWN_SIRE || target == UNKNOWN_DAM ||
        !horses.count(target)) {
        putValueToDB(target, ancestor, 0.0);
        return 0.0;
    }

    // 3) 同じ馬 => 1.0
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

    // 5) 再帰計算(父・母)
    const Horse& h = horses[target];
    double val = 0.5 * getBloodPercentageMemo_normal(h.Sire, ancestor, inStack)
        + 0.5 * getBloodPercentageMemo_normal(h.Dam, ancestor, inStack);

    inStack.erase(target);
    putValueToDB(target, ancestor, val);
    return val;
}

//--------------------------------------------------------------------
// 血量計算 (RocksDBにキャッシュ) - 逆版 (ancestor→target)
//   - すでにあるならDBから読み取り
//   - なければ計算してDBに書き込み
//--------------------------------------------------------------------
double getBloodPercentageMemo_reversed(const string& ancestor,
    const string& target,
    unordered_set<string>& inStack)
{
    // 同じく RocksDB のキーは (ancestor, target) とする
    double cachedVal;
    if (getValueFromDB(ancestor, target, cachedVal)) {
        return cachedVal;
    }

    // horsesにない or UNKNOWN => 0
    if (ancestor == UNKNOWN_SIRE || ancestor == UNKNOWN_DAM ||
        !horses.count(ancestor)) {
        putValueToDB(ancestor, target, 0.0);
        return 0.0;
    }

    // 同じ馬 => 1.0
    if (ancestor == target) {
        putValueToDB(ancestor, target, 1.0);
        return 1.0;
    }

    // 循環防止
    if (inStack.count(ancestor)) {
        putValueToDB(ancestor, target, 0.0);
        return 0.0;
    }
    inStack.insert(ancestor);

    // 再帰 (子孫を辿る形は本来複雑だが、ここでは単に親とみなして計算する例)
    // あなたの用途に応じてロジックが違う場合あり。
    // ここでは便宜上 "ancestorの父・母" => "ancestorの祖先" という扱いを反転
    // (実際の血統上は "逆探索" なので慎重に設計する必要がある)
    const Horse& ancHorse = horses[ancestor];
    double val = 0.5 * getBloodPercentageMemo_reversed(ancHorse.Sire, target, inStack)
        + 0.5 * getBloodPercentageMemo_reversed(ancHorse.Dam, target, inStack);

    inStack.erase(ancestor);
    putValueToDB(ancestor, target, val);
    return val;
}

//--------------------------------------------------------------------
// 行列形式CSV出力 (通常版:  row=allKeys, col=subset )
//   val = getBloodPercentageMemo_normal(row, col)
//--------------------------------------------------------------------
void saveCSVMatrix_Normal(const string& filename,
    const vector<string>& rowKeys,
    const vector<string>& colKeys)
{
    if (colKeys.empty()) {
        cout << "[saveCSVMatrix_Normal] skip because colKeys empty\n";
        return;
    }

    ofstream ofs(filename);
    if (!ofs.is_open()) {
        cerr << "[saveCSVMatrix_Normal] cannot open " << filename << endl;
        return;
    }
    ofs << fixed << setprecision(8);

    // ヘッダ行
    ofs << "HorseName";
    for (auto& ck : colKeys) {
        ofs << "," << keyToDisplayName[ck];
    }
    ofs << "\n";

    // 本文
    for (size_t i = 0; i < rowKeys.size(); i++) {
        const string& rk = rowKeys[i];
        size_t memUsage = getMemoryUsageMB();

        cout << "[通常版計算中] " << (i + 1) << "/" << rowKeys.size()
            << ": " << keyToDisplayName[rk]
            << " (Mem=" << memUsage << "MB)"
                << endl;

            ofs << keyToDisplayName[rk];

            for (auto& ck : colKeys) {
                unordered_set<string> inStack;
                double val = getBloodPercentageMemo_normal(rk, ck, inStack);
                if (fabs(val) < 1e-12) val = 0.0;
                ofs << "," << val;
            }
            ofs << "\n";
    }
    ofs.close();
    cout << "[saveCSVMatrix_Normal] " << filename << " 出力完了\n";
}

//--------------------------------------------------------------------
// 行列形式CSV出力 (逆版: row=allKeys, col=subset )
//   val = getBloodPercentageMemo_reversed(col, row)
//--------------------------------------------------------------------
void saveCSVMatrix_Reversed(const string& filename,
    const vector<string>& rowKeys,
    const vector<string>& colKeys)
{
    if (colKeys.empty()) {
        cout << "[saveCSVMatrix_Reversed] skip because colKeys empty\n";
        return;
    }

    ofstream ofs(filename);
    if (!ofs.is_open()) {
        cerr << "[saveCSVMatrix_Reversed] cannot open " << filename << endl;
        return;
    }
    ofs << fixed << setprecision(8);

    // ヘッダ行
    ofs << "HorseName";
    for (auto& ck : colKeys) {
        ofs << "," << keyToDisplayName[ck];
    }
    ofs << "\n";

    // 本文
    for (size_t i = 0; i < rowKeys.size(); i++) {
        const string& rk = rowKeys[i];
        size_t memUsage = getMemoryUsageMB();

        cout << "[逆版計算中] " << (i + 1) << "/" << rowKeys.size()
            << ": " << keyToDisplayName[rk]
            << " (Mem=" << memUsage << "MB)"
                << endl;

            ofs << keyToDisplayName[rk];

            for (auto& ck : colKeys) {
                unordered_set<string> inStack;
                double val = getBloodPercentageMemo_reversed(ck, rk, inStack);
                if (fabs(val) < 1e-12) val = 0.0;
                ofs << "," << val;
            }
            ofs << "\n";
    }
    ofs.close();
    cout << "[saveCSVMatrix_Reversed] " << filename << " 出力完了\n";
}

//--------------------------------------------------------------------
// 年代区分 (0~1800, 1801~1850, 1851~1900, 1901~1950, 1951~2000, 2001~ )
//--------------------------------------------------------------------
string getYearBucket(int y) {
    if (y == INT_MIN || y <= 1800) {
        return "0_1800";
    }
    else if (y <= 1850) {
        return "1801_1850";
    }
    else if (y <= 1900) {
        return "1851_1900";
    }
    else if (y <= 1950) {
        return "1901_1950";
    }
    else if (y <= 2000) {
        return "1951_2000";
    }
    else {
        return "2001_";
    }
}

//--------------------------------------------------------------------
// メイン
//--------------------------------------------------------------------
int main()
{
    cout << "現在の実行ディレクトリ: " << fs::current_path() << endl;

    try {
        // 1) 血統データCSV読み込み (絶対パス or 相対パス)
        loadBloodlineCSV("D:/AI/C++/blood_cache_db");

        // 2) RocksDBオープン (データ永続化)
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

        // 4) 50年刻みで年代区分に分割
        //    (  0_1800, 1801_1850, 1851_1900, 1901_1950, 1951_2000, 2001_ )
        //    各区分 => vector<string> subset
        unordered_map<string, vector<string>> subsets;

        for (auto& k : allKeys) {
            int y = horses[k].YearInt;
            string bucket = getYearBucket(y); // 50年区切り
            subsets[bucket].push_back(k);
        }

        // 例： subsets["0_1800"], subsets["1801_1850"], ... subsets["2001_"]

        // 5) それぞれ (通常版, 逆版) の行列出力
        //   ex) "blood_percentage_0_1800_normal.csv", "blood_percentage_0_1800_reversed.csv"
        for (auto& kv : subsets) {
            const string& bucketName = kv.first;           // "0_1800" etc
            const vector<string>& bucketKeys = kv.second; // その年代に属する馬

            // 行=allKeys, 列=bucketKeys (通常版)
            {
                string fname = "D:/AI/C++/out/blood_percentage_" + bucketName + "_normal.csv";
                saveCSVMatrix_Normal(fname, allKeys, bucketKeys);
            }
            // 行=allKeys, 列=bucketKeys (逆版)
            {
                string fname = "D:/AI/C++/out/blood_percentage_" + bucketName + "_reversed.csv";
                saveCSVMatrix_Reversed(fname, allKeys, bucketKeys);
            }
        }

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
