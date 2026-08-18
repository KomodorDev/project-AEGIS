# Runtime Flows

**Status: Accepted invariants with illustrative participants and proposed failure branches.** These flows make the serialized data-plane execution model concrete without defining final C++ interfaces or deferred state transitions.

Return to the [architecture overview](../architecture.md) or read [ADR-0001](../decisions/0001-serialized-data-plane-execution.md).

- Solid arrows (`->>`) represent interactions completed in the current execution context. Within the data plane, they run on the single serialized executor without an intervening general-purpose queue or thread hop.
- Dashed arrows (`-->>`) represent asynchronous transport, a later executor turn or off-path control-plane delivery.
- Participant and message names are illustrative rather than final C++ interfaces.
- Ordering and ownership boundaries are architectural constraints.

## 1. Market-Data Delivery

This steady-state flow begins after the venue adapter has established the exchange connection and subscribed to the required native market-data channels. That exchange subscription is separate from the internal bot subscriptions matched by the subscription dispatcher below.

```mermaid
sequenceDiagram
    autonumber
    participant X as Exchange
    participant V as Venue market-data session
    participant P as Native parser
    participant N as Market-data normalizer
    participant D as Subscription dispatcher
    participant B as Bot / strategy

    X-->>V: Pushed native market-data frame received asynchronously
    Note over V,B: A later data-plane executor turn begins
    V->>P: Parse and validate native frame

    alt Frame is invalid or unsupported
        P->>V: Inline failure result, no MarketEvent
        Note over V,D: Diagnostic and recovery policy remains open
    else Frame is valid
        P->>N: Parsed native message
        N->>D: Normalized MarketEvent
        D->>D: Match configured subscriptions

        loop Each matching bot subscription
            D->>B: on_market_event(event, context)
            Note right of B: Callback runs to completion and must not block or re-enter
        end
    end
```

A strategy may submit an order while handling `on_market_event`; that continues directly into the submission flow below on the same executor turn.

## 2. Order Submission

```mermaid
sequenceDiagram
    autonumber
    participant B as Bot / strategy callback
    participant S as Order submission
    participant A as Route authorization
    participant G as Inline pre-trade risk guard
    participant O as OMS
    participant V as Venue / account session
    participant X as Exchange

    Note over B,V: One data-plane executor turn with no pre-risk queue or thread hop
    B->>S: submit OrderRequest
    S->>A: Resolve and authorize a configured execution route

    alt No permitted route
        A->>S: Route rejection
        S->>B: Local rejected SubmitResult
    else Authorized route context
        A->>S: Venue, account and instrument context
        S->>G: check_and_reserve(bot, request, route)
        G->>G: Check installed budgets and modes, then reserve exposure atomically

        alt Risk check rejects
            G->>S: Risk rejection
            S->>B: Local rejected SubmitResult
        else Risk check approves
            G->>S: Approval with reservation context
            S->>O: Admit request with route and reservation

            alt OMS cannot admit locally
                O->>S: Non-admission result
                Note over G,O: Reserved capacity must not leak, exact transition ownership remains open
                S->>B: Local rejected SubmitResult
            else OMS admits request
                O->>V: Submit admitted order
                V->>V: Encode exchange-native message
                V->>V: Request non-blocking socket-write initiation

                alt Encoding or local initiation fails immediately
                    V->>O: Local submission failure
                    Note over G,O: Reserved capacity must not leak, exact transition ownership remains open
                    O->>S: Local failure result
                    S->>B: Local failed SubmitResult
                else Write initiation is requested locally
                    V->>O: Local initiation outcome
                    O->>S: Local submission outcome
                    S->>B: Local SubmitResult
                    V-->>X: Native order bytes through asynchronous transport
                end
            end
        end
    end
```

The exact successful `SubmitResult` states and timing remain open; no local result is an exchange acknowledgement. A bounded session-local write sequencer may eventually be required after OMS admission, but its admission, capacity and overload semantics remain open. It must not become a pre-risk service queue.

Route authorization is mandatory, but whether an `OrderRequest` names a route or another configuration-owned mechanism resolves its account remains open. The precise reservation transitions on OMS non-admission and local transport failure also remain open; the invariant is that reserved capacity cannot be silently leaked.

## 3. Acknowledgements, Rejections and Fills

```mermaid
sequenceDiagram
    autonumber
    participant X as Exchange
    participant V as Venue / account session
    participant O as OMS
    participant R as Reservation / exposure state
    participant I as Position / inventory state
    participant B as Bot / strategy
    participant C as Risk coordinator

    X-->>V: Native acknowledgement, rejection or fill
    Note over V,B: A later data-plane executor turn begins
    V->>V: Parse, normalize and correlate native message
    V->>O: Normalized OrderEvent

    alt Exchange acknowledgement
        O->>O: Reconcile acknowledgement with OMS state
        O->>R: Apply acknowledgement consequence, if any
    else Exchange rejection
        O->>O: Reconcile rejection with OMS state
        O->>R: Apply rejection consequence to reserved exposure
    else Fill, including a partial fill
        O->>O: Reconcile fill with OMS order state
        O->>R: Apply fill consequence to reservation and exposure
        O->>I: Apply fill with bot, desk and firm attribution
    end

    Note over R,I: Owner-local state changes complete before another risk decision
    O->>B: on_order_event(event, context)
    R-->>C: Exposure and reservation observation, if any
    I-->>C: Position and inventory observation, if any
    Note over R,C: Control-plane reporting is asynchronous and cannot delay the callback path
```

Every private order or execution event shown here passes through OMS reconciliation. A fill cannot update a reporting ledger while bypassing OMS or the immediate position state used by subsequent risk decisions. The exact OMS states and reservation effects for acknowledgements, rejections, partial fills, cancellations and out-of-order exchange events remain open.

## 4. Risk Snapshot Publication and Enforcement

```mermaid
sequenceDiagram
    autonumber
    participant D as Data-plane state reporting
    participant U as Control-plane API / operator UI
    participant C as Risk coordinator
    participant E as Serialized data-plane executor
    participant G as Inline pre-trade risk guard
    participant B as Bot / strategy
    participant S as Order submission

    alt Aggregated risk changes an allocation
        D-->>C: Position, reservation and exposure observation
        C->>C: Recalculate hierarchical allocations
    else Authorized risk-mode change
        U->>C: Set Normal, ReduceOnly or Halted
        C->>C: Apply requested hierarchical mode change
    end

    C->>C: Build complete immutable budget and mode snapshot
    C->>C: Assign monotonic revision n
    C-->>E: Publish snapshot revision n

    Note over E,G: Delivery cannot interrupt a callback, so adoption occurs between callbacks
    E->>G: Offer complete snapshot revision n

    alt Revision n is newer than the installed revision
        G->>G: Replace installed snapshot as one logical update
    else Revision n is stale or duplicated
        G->>G: Ignore snapshot revision n
    end

    E->>B: Invoke the next strategy callback
    B->>S: submit OrderRequest
    S->>S: Authorize configured execution route
    S->>G: check_and_reserve using installed snapshot and current exposure
    G->>G: Enforce budgets and modes, then reserve or reject

    alt Risk rejects
        G->>S: Inline risk rejection
        S->>B: Local rejected SubmitResult
    else Risk approves and reserves
        G->>S: Approval with reservation context
        Note over B,S: Continue through OMS and venue write initiation as shown in Flow 2
    end
    Note over S,G: No synchronous call to the coordinator or UI occurs
```

Snapshots are complete so a callback never observes a partially updated budget or mode hierarchy. Monotonic revisions prevent delayed control-plane delivery from reinstalling older authority.

## Cross-Flow Invariants

- One dedicated thread and serialized executor owns all mutable v1 data-plane state shown here.
- Data-plane handlers run to completion, remain non-blocking and are non-reentrant.
- Strategy submission, route authorization, risk check and reservation, OMS admission, native encoding and the write-initiation request remain one direct executor-local path.
- The OMS cannot be bypassed by outbound orders or inbound private-order events.
- Fill-driven position and exposure changes are visible before a bot receives the fill event or can make another risk decision.
- Control-plane aggregation, monitoring and UI work never blocks the latency-sensitive path.
- Exchange acknowledgements, rejections, fills and socket completions occur on later executor turns.

## Open Items

- Execution-route identity and account-selection interface.
- Detailed OMS states, identifiers, correlation rules and event-order handling.
- Reservation transitions for every acknowledgement, rejection, partial fill, cancellation, timeout and local failure.
- Exact hierarchical semantics of `ReduceOnly` and `Halted`, including existing-order cancellation and budget reductions below current exposure.
- Session write sequencing, inbound and outbound overload policy and cross-plane reporting backpressure.
- Reconnect, recovery, exchange reconciliation and persistence behavior.
- Asynchronous-I/O library and concrete executor primitives.

Reconnect and reconciliation sequences are intentionally deferred until the OMS state model and recovery policy are decided.
