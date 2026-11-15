#!/usr/bin/env python3
"""Compare replay actions with queue_ahead=0 and 5000.

This is a small mocked queue-feedback scenario, not a matching engine or a
latency benchmark. Both runs receive the same valid-tick books. The mock venue
acknowledges one emitted limit order as resting, then supplies queue feedback.

    python3 scripts/queue_replay_test.py
    python3 scripts/queue_replay_test.py --callback ack
    python3 scripts/queue_replay_test.py --callback queue --expect-different
"""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def book(bid, ask, bid_qty=6000, ask_qty=1500, symbol="TEST"):
    mid = round((bid + ask) / 2, 2)
    return {
        "type": "book_snapshot", "symbol": symbol, "asset_type": "equity",
        "bids": [[bid, bid_qty]], "asks": [[ask, ask_qty]],
        "mid_price": mid, "spread": round(ask - bid, 2), "ref_price": mid,
        "microprice": (ask * bid_qty + bid * ask_qty) / (bid_qty + ask_qty),
        "obi": (bid_qty - ask_qty) / (bid_qty + ask_qty),
    }


def replay(bot, events):
    result = subprocess.run(
        [str(bot), "--replay"], cwd=ROOT,
        input="".join(json.dumps(event) + "\n" for event in events),
        text=True, capture_output=True, check=True, timeout=10,
    )
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    if len(replies) != len(events) or any(
            not isinstance(reply, list) for reply in replies):
        raise ValueError("Expected one JSON array per replay input event")
    return replies, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bot", type=Path,
                        default=ROOT / "hft/cpp_client/build/hft_bot")
    parser.add_argument("--callback", choices=("both", "ack", "queue"),
                        default="both", help="which callback's queue position varies")
    parser.add_argument("--expect-different", action="store_true",
                        help="exit 1 if the two action sequences are identical")
    args = parser.parse_args()
    bot = args.bot.resolve()
    if not bot.is_file():
        parser.error(f"No executable at {bot}; build the client first")

    # Warm the strategy, then move the midpoint to elicit an initial order.
    primer = [book(100.00, 100.04), book(100.01, 100.05)]
    initial, _ = replay(bot, primer)
    orders = [order for reply in initial for order in reply]
    order = next((order for order in orders
                  if order.get("type") == "place_order" and
                  order.get("order_type") == "limit"), None)
    if order is None:
        raise ValueError("Primer elicited no limit order; adapt it to the strategy")
    side, price, qty = order["side"], order["price"], order["quantity"]
    if side not in ("buy", "sell") or not math.isfinite(price) or qty <= 0:
        raise ValueError("Primer must elicit a valid limit order")

    # Model a quote move before arrival so this order can rest at its price.
    # Keep total level quantity equal in both cases and sufficient for 5000 ahead.
    level_qty = max(6000, 5000 + qty)
    other_qty = max(1, level_qty // 4)
    if side == "buy":
        following = book(price, round(price + 0.04, 2),
                         level_qty, other_qty, order["symbol"])
    else:
        following = book(round(price - 0.04, 2), price,
                         other_qty, level_qty, order["symbol"])

    outputs = []
    for ahead in (0, 5000):
        ack = {
            "type": "order_ack", "order_id": "queue-test-1",
            "symbol": order["symbol"], "side": side, "price": price,
            "quantity": qty, "level_qty": level_qty,
            "queue_ahead": 0 if args.callback == "queue" else ahead,
        }
        feedback = [ack]
        if args.callback != "ack":
            feedback.append(dict(ack, type="queue_update", queue_ahead=ahead))
        replies, log = replay(bot, primer + feedback + [following])
        if replies[:len(primer)] != initial:
            raise ValueError("Primer output changed; the comparison is not controlled")
        actions = [action for reply in replies[len(primer):] for action in reply]
        outputs.append(actions)
        print(f"queue_ahead={ahead} ({args.callback}), level_qty={level_qty}")
        for line in log.splitlines():
            if line.startswith(("[ack]", "[queue]")):
                print(f"  {line}")
        print("  actions after feedback:", json.dumps(actions, sort_keys=True))

    different = outputs[0] != outputs[1]
    print("RESULT:", "DIFFERENT" if different else "SAME")
    if not different:
        print("No queue-dependent action was observed in this scenario.")
    return 1 if args.expect_different and not different else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"queue_replay_test: {error}", file=sys.stderr)
        sys.exit(2)
