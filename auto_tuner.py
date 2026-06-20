import os
import re
import subprocess
import random
import time

# --- 設定區 ---
CPP_FILE = "src/policy/submission.cpp"  
MAKE_CMD = "mingw32-make"               
MATCH_GAMES = 10                        # 縮減局數以加速演化 (10局)
TIME_LIMIT = 200                        # 減少每步思考時間 (200ms)，足夠評估參數優劣

# 我們要訓練的參數與其安全範圍 (Min, Max)
PARAMS_CONFIG = {
    "TUNE_HANGING_PENALTY": (5, 50),
    "TUNE_BAD_EXCHANGE": (5, 50),
    "TUNE_PROTECT_BONUS": (1, 15),
    "TUNE_FORK_PENALTY": (5, 50),
    "TUNE_NO_SHIELD": (5, 40),
    "TUNE_XRAY_THREAT": (5, 40),
    "TUNE_FATAL_OPEN_FILE": (10, 80)
}

def read_cpp_params():
    with open(CPP_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    
    current_params = {}
    for param in PARAMS_CONFIG.keys():
        match = re.search(fr"#define\s+{param}\s+(\d+)", content)
        if match:
            current_params[param] = int(match.group(1))
    return current_params

def write_cpp_params(params):
    with open(CPP_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    
    for param, value in params.items():
        content = re.sub(fr"#define\s+{param}\s+\d+", f"#define {param} {value}", content)
        
    with open(CPP_FILE, "w", encoding="utf-8") as f:
        f.write(content)

def compile_cpp():
    result = subprocess.run(MAKE_CMD, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.returncode == 0

def run_tournament():
    cmd = [
        "python", "-m", "cli.cli",
        "--white", "build/Knight_merge-ubgi.exe", 
        "--black", "build/boss-ubgi.exe",         
        "--games", str(MATCH_GAMES),
        "--time", str(TIME_LIMIT)
    ]
    
    # 執行對戰並捕捉所有輸出 (包含標準輸出與錯誤輸出)
    process = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    output = process.stdout + process.stderr
    
    wins = 0
    losses = 0
    
    # 🔍 強效解析雷達：自動捕捉各種可能的勝負格式
    # 方法 1: 尋找單局模式的 winner=0 (白方贏) 與 winner=1 (黑方贏)
    wins += output.count("winner=0")
    losses += output.count("winner=1")
    
    # 方法 2: 尋找常見的總結表 "White wins: X" / "Black wins: Y"
    if wins + losses == 0:
        m_white = re.search(r"White\s*wins?:\s*(\d+)", output, re.IGNORECASE)
        m_black = re.search(r"Black\s*wins?:\s*(\d+)", output, re.IGNORECASE)
        if m_white and m_black:
            wins = int(m_white.group(1))
            losses = int(m_black.group(1))
            
    # 方法 3: 尋找 "Player 0 wins: X" / "Player 1 wins: Y"
    if wins + losses == 0:
        m_p0 = re.search(r"Player\s*0\s*wins?:\s*(\d+)", output, re.IGNORECASE)
        m_p1 = re.search(r"Player\s*1\s*wins?:\s*(\d+)", output, re.IGNORECASE)
        if m_p0 and m_p1:
            wins = int(m_p0.group(1))
            losses = int(m_p1.group(1))

    total = wins + losses
    
    # 🚨 防呆機制：如果還是抓不到，直接把系統輸出印出來給人類看！
    if total == 0:
        print("\n⚠️ [警告] 無法解析勝負分數！cli.py 的原始輸出結尾如下：")
        print("=" * 50)
        # 只印出最後 1000 個字元避免洗版
        print(output[-1000:]) 
        print("=" * 50)
        return 0.0, 0, 0
        
    win_rate = (wins / total) if total > 0 else 0
    return win_rate, wins, losses

def main():
    print("🚀 啟動 AI 自動煉蠱系統...")
    best_params = read_cpp_params()
    if not best_params:
        print("❌ 找不到參數！請確認 submission.cpp 中是否有 #define TUNE_... 的設定。")
        return
    
    print(f"📊 編譯初始版本中...")
    if not compile_cpp():
        print("❌ 編譯失敗，請檢查 C++ 語法！")
        return
        
    print(f"⚔️ 進行初始能力測試 ({MATCH_GAMES} 局)...")
    best_win_rate, w, l = run_tournament()
    
    if w + l == 0:
        print("🛑 測試中斷：無法讀取成績，請檢查上方的原始輸出。")
        return
        
    print(f"🏆 初始勝率: {best_win_rate*100:.1f}% ({w}勝 {l}敗)\n")
    
    generation = 1
    while True:
        print(f"--- 🧬 第 {generation} 代變異 ---")
        test_params = best_params.copy()
        mutate_count = random.randint(1, 2)
        params_to_mutate = random.sample(list(PARAMS_CONFIG.keys()), mutate_count)
        
        for param in params_to_mutate:
            min_val, max_val = PARAMS_CONFIG[param]
            delta = random.choice([-5, -3, -1, 1, 3, 5])
            new_val = max(min_val, min(max_val, test_params[param] + delta))
            test_params[param] = new_val
            print(f"   🔧 突變 {param}: {best_params[param]} -> {new_val}")
            
        write_cpp_params(test_params)
        if not compile_cpp():
            print("   ❌ 編譯失敗，跳過此代...")
            write_cpp_params(best_params)
            continue
            
        print(f"   ⚔️ 測試新參數中...")
        start_time = time.time()
        win_rate, w, l = run_tournament()
        elapsed = time.time() - start_time
        
        print(f"   結果: {win_rate*100:.1f}% ({w}勝 {l}敗) - 耗時 {elapsed:.1f}秒")
        
        if win_rate > best_win_rate:
            print("   🌟 發現更強的參數組合！進化成功！")
            best_win_rate = win_rate
            best_params = test_params.copy()
        else:
            print("   💀 突變未達標，參數還原。")
            write_cpp_params(best_params) 
            
        generation += 1
        print("")

if __name__ == "__main__":
    main()