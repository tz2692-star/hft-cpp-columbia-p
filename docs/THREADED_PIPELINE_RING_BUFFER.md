# Threaded pipeline and ring-buffer design

## Live pipeline

```text
                  WebSocket / receive callback thread
                  decode frame, copy callback data
                               |
                               | publish pipeline::Event
                               v
                inbound SPSC ring (capacity 1024)
                               |
                               v
                    strategy consumer thread
                  update state, make decisions
                               |
                               | publish StrategyOrder
                               v
                 outbound SPSC ring (capacity 1024)
                               |
                               v
                      order sender thread
              encode message and call WebSocket send
```

The thread running `main()` waits for the live client to stop. IXWebSocket
provides the background thread that invokes the receive callback. The bot
starts a strategy consumer and an order sender in [SpreadCaptureBot](../hft/cpp_client/include/spread_capture_bot.hpp).
The receive callback decodes messages in [ArenaClient](../hft/cpp_client/include/arena_client_impl.hpp),
then the bot copies relevant callback data into fixed-size pipeline events and
publishes them to the inbound queue.

The strategy consumer owns and updates [SpreadStrategy](../hft/cpp_client/include/spread_strategy.hpp),
including symbol state, local books, and queue state. For a live order decision,
it copies a fixed-size `StrategyOrder` into the outbound queue. The sender thread
does the transport work separately: it converts the symbol to its reusable
string buffer, encodes the order through the client's send helpers, and asks
IXWebSocket to send it.

This separation means strategy decisions are **not run inside `on_book` on the
receive callback thread**. Parsing and message decoding still happen on that
receive thread before events reach the strategy consumer.

## SPSC queues and ownership

Both handoffs use [TypedSpscRing](../hft/cpp_client/include/typed_spsc_ring.hpp),
a bounded, power-of-two, single-producer/single-consumer ring:

| Queue | Producer | Consumer | Payload |
|---|---|---|---|
| Inbound | WebSocket receive callback | Strategy consumer | `pipeline::Event` |
| Outbound | Strategy consumer | Order sender | `StrategyOrder` |

Each queue has capacity 1024 and stores its payloads in a preallocated array.
The producer copies into a free slot and publishes the tail index with a
release store. The consumer observes that index with an acquire load, copies
the payload out, and releases the slot by advancing the head. Head and tail
indices are separately wrapped in 64-byte-aligned structures; the slot array
is also 64-byte aligned. The implementation statically requires lock-free
atomic index types.

There is exactly one producer and one consumer per queue in live mode. Do not
add another producer or consumer without changing the synchronization design.
The ring does not allocate, lock, overwrite a full slot, or wait for space.
When the inbound queue is full, the channel latches a fault and stops the bot
rather than continue with incomplete strategy state. When the outbound queue
is full, the bot stops and reports an outbound queue overflow after shutdown.

## Decision-path properties and limits

The strategy computation and its outbound handoff use fixed-size data,
preallocated symbol/state storage, and the SPSC queue. That section of the live
path is designed to be **allocation-free and lock-free**. It does not include
the whole receive-to-exchange path:

- JSON decoding and receive-side callback work happen before the inbound queue.
- Order serialization, message-pool use, and WebSocket sending happen on the
  sender thread, outside the decision path; transport internals may allocate or
  use locks.
- Shutdown reporting and diagnostic output are not part of the decision path.
- Lock-free atomic indices do not make unrelated transport or runtime code
  lock-free.

In replay mode there is no live consumer/sender pipeline: dispatch and
strategies run inline, and generated orders are captured synchronously for the
replay response.

## CPU affinity on Linux

The bot optionally pins the receive callback, strategy consumer, and order
sender to logical CPU IDs supplied by `HFT_RX_CPU`, `HFT_STRATEGY_CPU`, and
`HFT_SENDER_CPU`. Set all three to distinct IDs or leave all unset. The receive
thread is pinned when it first enters a bot callback; the strategy and sender
threads pin themselves at startup. A failed pin is reported and stops
processing. The main thread is not pinned.

For example, after choosing three distinct, online, process-allowed CPUs:

```bash
HFT_RX_CPU=10 HFT_STRATEGY_CPU=12 HFT_SENDER_CPU=14 ./build/hft_bot
```

Affinity is not CPU isolation. The operating system may still schedule other
tasks on a pinned CPU unless the host has separately configured CPU isolation
and housekeeping/IRQ placement. The selected CPUs must be online and allowed
for the process. The application does not configure boot parameters or isolate
CPUs itself.

## Shutdown order

When live `run()` returns, the WebSocket receive producer has stopped. The bot
marks the inbound producer done and joins the strategy consumer, which drains
published events. It then marks strategy production done and joins the sender,
which drains queued orders before exiting. Worker errors, pipeline faults, and
outbound queue overflow are surfaced after the corresponding threads are
joined.
