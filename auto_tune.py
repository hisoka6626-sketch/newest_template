import subprocess
import re
import sys

# === 實驗設定 ===
BASELINE_EXE = "build/boss-ubgi.exe" # 舊版 AI
TEST_EXE = "build/minichess-ubgi.exe"         # 改了參數的新版 AI
ALGO = "submission"                      # 演算法名稱
TIME_LIMIT = "2000"                       # 【修正】必須是毫秒整數！500 代表 0.5 秒
GAMES_PER_SIDE = 100                      # 黑白各下 50 局 (共 100 局)

def run_match(white_exe, black_exe, num_games):
    # 【核心修復】：自動判斷選手身分並發放正確的演算法武器！
    white_algo = "pvs" if "boss" in white_exe.lower() else "submission"
    black_algo = "pvs" if "boss" in black_exe.lower() else "submission"

    cmd = [
        sys.executable, "-m", "cli.cli",
        "--white", white_exe,
        "--black", black_exe,
        "--white-algo", white_algo,   # 給予白方正確的武器
        "--black-algo", black_algo,   # 給予黑方正確的武器
        "--time", TIME_LIMIT,
        "--games", str(num_games),
        "--quiet"
    ]
    print(f"⚔️ 開戰：白方({white_exe} [{white_algo}]) vs 黑方({black_exe} [{black_algo}]) 進行 {num_games} 局...")
    
    result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')

    white_wins, black_wins, draws = 0, 0, 0
    match = re.search(r"White wins:\s*(\d+)\s*Black wins:\s*(\d+)\s*Draws:\s*(\d+)", result.stdout)
    
    if match:
        white_wins = int(match.group(1))
        black_wins = int(match.group(2))
        draws = int(match.group(3))
    else:
        print(f"⚠️ {white_exe} vs {black_exe} 測試失敗。")
        print("錯誤訊息:\n", result.stderr)

    return white_wins, black_wins, draws

def main():
    print("🚀 === 開始自動化魔法參數天擇測試 === 🚀\n")
    
    # 階段 1：Test 執白
    w_wins1, b_wins1, draws1 = run_match(TEST_EXE, BASELINE_EXE, GAMES_PER_SIDE)
    test_wins_as_white = w_wins1
    test_losses_as_white = b_wins1

    # 階段 2：Test 執黑
    w_wins2, b_wins2, draws2 = run_match(BASELINE_EXE, TEST_EXE, GAMES_PER_SIDE)
    test_wins_as_black = b_wins2
    test_losses_as_black = w_wins2

    # 統計
    total_wins = test_wins_as_white + test_wins_as_black
    total_losses = test_losses_as_white + test_losses_as_black
    total_draws = draws1 + draws2

    print("\n📊 === 100 局測試結果總結 === 📊")
    print(f"新參數 (Test) 總勝場: {total_wins}")
    print(f"舊參數 (Baseline) 總勝場: {total_losses}")
    print(f"總平局數: {total_draws}")

    if total_wins > total_losses:
        print("\n🎉 結論：新的魔法參數碾壓了舊版！請保留這些改動！")
    elif total_wins < total_losses:
        print("\n❌ 結論：新參數效果不佳，請退回上一個版本。")
    else:
        if total_wins == 0 and total_losses == 0 and total_draws == 0:
            print("\n⚠️ 測試仍未成功執行，請往上查看錯誤訊息。")
        else:
            print("\n⚖️ 結論：實力相當，建議微調數值後再測一次。")

if __name__ == "__main__":
    main()