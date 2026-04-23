# Limceron

**The language where agents cannot exist without guardrails.**
> Agents need an operating system, not a library.


## The Problem

You deploy an AI agent on Friday. By Monday:

- It made 40,000 LLM calls because nobody set a budget
- It concatenated user input into the system prompt and got jailbroken
- It called `http://169.254.169.254` and leaked your AWS credentials
- It ran `rm -rf /data` because the LLM hallucinated a shell command
- Its accuracy drifted from 90% to 60% and nobody noticed

- **Nothing crashed.**
- **Nothing alerted you.**
- **It just got worse.**

Every framework gives you tools to build agents. None give you tools to **trust** them.

The root cause: agents need an operating system, not a library. They need a kernel that controls what they can access, how much they can spend, how much uncertainty they can tolerate, and what happens when they drift. They need Limceron.

---

## Why This Happens

Every existing framework treats agents like **code you write**.

They are not.

Agents are:

* non-deterministic
* probabilistic
* capable of unsafe behavior by default

You don’t need a framework.

You need:

* constraints
* enforcement
* observability
* automatic failure

You need an **operating system for agents**.

---

## What Limceron Is

Limceron is a programming language where:

* **Capabilities are enforced at compile-time**
* **Budgets are part of the syntax**
* **Uncertainty is measured and bounded**
* **Agents stop themselves when they become unreliable**

Not middleware.
Not decorators.
Not runtime patches.

👉 If it’s not declared, it doesn’t compile.

---

## Three Pillars

```
capability      controls WHAT the agent can access (endpoints, binaries, paths)
budget          controls HOW MUCH the agent can spend (tokens, cost, time)
entropy_budget  controls HOW MUCH UNCERTAINTY the agent can tolerate
```

Every other framework bolts these on as middleware. In Limceron, the compiler refuses to produce a binary without them.

## What It Looks Like

A complete agent in 12 lines. If you can read pseudocode, you can read Limceron:

```limceron
agent Bot {
    capabilities: [llm.classify, data.read]
    model: "gpt-4o"
    
    budget: { 
    	max_tokens: 10000, 
    	max_cost: 1.00 
    }

    fn classify(comment: string) -> Result {
        let result = ask(comment)
        if result.confidence > 0.85 { 
        	result 
        } else { 
        	"REVIEW:" + result 
        }
    }
}
```

### What just happened?

* `ask()` returns **confidence**
* low-confidence outputs are **automatically escalated**
* spending is **bounded**
* access is **restricted**

No extra code. No wrappers.

No boilerplate. No decorators. No YAML. The `ask()` call returns a confidence score on every response -- Shannon entropy of the LLM's probability distribution, normalized to [0, 1]. High confidence auto-commits. Low confidence escalates to a human. You never have to guess whether your agent is guessing.

## You Can Start Even Simpler (Markdown)

You don't need to learn a new language. A `.lceron.md` file compiles to the **exact same binary**:

```markdown
# agent Bot

> Classify comments into categories.

## capabilities
- llm.classify
- data.read

## model
gpt-4o

## budget
- max_tokens: 10000
- max_cost: 1.00
```
👉 This compiles to the exact same binary.

Markdown is source code. A product manager can write the spec, a developer can extend it with code blocks, and the compiler treats both the same. Progressive disclosure: start with Markdown, add code when you need it, graduate to full `.lceron` when you're ready.

## Production-Ready From Day One

**What happens when your agent goes to production?**

In every other framework, you add health checks after the first outage. You add metrics after you realize you're flying blind. You add signal handlers after the first data corruption. You add retry logic after the first transient failure.

In Limceron, the language has these built in:

```limceron
health { 
	ready: model.loaded(), live: true, port: 9090 
}

metrics {
    counter processed_total "Records categorized"
    histogram latency_ms "Inference latency"
    port: 9091
}

progress { total: count, current: processed }
```

Retry with exponential backoff is a builtin:

```limceron
let result = retry_ask_backoff(agent, comment, 3)  // 3 retries, exponential
```

Graceful shutdown with resource cleanup:

```limceron
defer { db.close(conn) }   // LIFO cleanup -- runs on exit, panic, or signal
```

Other frameworks make you remember to add these. Limceron makes you unable to forget them.

## The Agent Knows When It Doesn't Know

You deploy a model. It degrades silently. By the time someone notices, 10,000 records have wrong categories. The model didn't crash -- it just got less accurate, and nothing in your stack told you.

In Limceron, the agent has self-awareness about its own uncertainty and autonomously decides to stop:

```limceron
agent Categorizer {
    model: "bert"
    endpoint: "local"                    // ONNX model, no API, no GPU
    entropy_budget: {
        max_avg_entropy: 0.7             // stop if predictions become uncertain
        max_low_confidence: 0.20         // stop if >20% are low confidence
        max_drift: 0.15                  // stop if distribution shifts
    }
}
```

**`endpoint: "local"`** routes `ask()` to the ONNX model instead of HTTP. Same agent code, zero changes. Switch from a cloud LLM to a local BERT model by changing one line.

**`entropy_budget`** is an automatic circuit breaker. The runtime tracks Shannon entropy and confidence of every prediction. Three conditions are checked after each `ask()`:

1. **avg_entropy > 0.7** -- the model is confused on average. Predictions are spread across categories instead of concentrating on one. Halt.
2. **>20% low confidence** -- too many individual predictions fall below the confidence threshold. This isn't one bad input -- it's a systematic problem. Halt.
3. **drift > 0.15** -- the distribution of outputs has shifted from the baseline (measured by KL-divergence). The data changed, or the model changed. Either way, halt.

When any condition is violated, `ask()` returns `Error("entropy budget exceeded: ...")` -- the agent stops itself. No human needed to notice. No monitoring dashboard needed to fire an alert. The agent knew.

**Real result:** thousands of radiology records, 95% processed by BERT (~10ms each), 5% sent to a human review table. The agent knew which 5% it couldn't handle -- and it knew that if the 5% ever became 25%, something was wrong and it should stop entirely.

Other frameworks let you deploy a degrading model and hope someone notices. Limceron agents stop themselves.

## Real Exit Case: Patana Medical Categorizer

This is not a demo. This ran against production data.

A Limceron agent categorized **thousands of radiology return records** from medical centers using a fine-tuned BERT model (Patana, 110M params). Results: **93.8% F1 score**, **~10ms per inference**, no GPU required.

```limceron
use driver("mysql") as db
use model("bert-model.onnx") as bert

capability filesystem {
    allow path "/tmp/**" { mode: [read, write] }
    allow path "/opt/models/**" { mode: [read] }
    deny path "/etc/**"
    default: deny
}

taint user_input

fn main() -> Result {
    let conn = db.connect(env("DB_HOST"), env("DB_USER"), env("DB_PASS"), env("DB_NAME"), 3306)
    let rows = db.query(conn, "SELECT id, Comment FROM staging WHERE Category IS NULL LIMIT 500")

    for i in 0..db.row_count(rows) {
        let comment = db.get(rows, i, "Comment")
        let prediction = bert.predict(comment)

        if prediction.confidence >= 85 {
            let safe_cat = sql_escape(prediction.label)
            db.execute(conn, "UPDATE staging SET Category = '" + safe_cat + "' WHERE id = " + id)
        }
    }

    db.free(rows)
    db.close(conn)
}
```

| Metric | LLM (Qwen3-8B) | Fine-tuned BERT |
|---|---|---|
| Accuracy vs human | 82.6% | **93.1%** |
| Auto-commit accuracy (>95% conf) | N/A | **98.8%** |
| Speed per record | 200ms (GPU) | **10ms (CPU)** |
| Model size | 16 GB | **440 MB** |
| Monthly cost | ~$730 (GPU 24/7) | **~$36 (CPU)** |

The ONNX path gives you the most precise confidence scores: a softmax distribution over every possible category, not just the first token's logprobs. When BERT says 96% confidence, it means 96% of the probability mass is on one category.

Full source: `examples/language/25_exit_case_patana.lceron`

## What's Inside

Every feature exists to prevent something you fear.

| What you get | What it prevents |
|---|---|
| `capability { allow/deny }` -- compile-time access control | Unauthorized API calls, SSRF, arbitrary shell execution |
| `budget { max_tokens, max_cost }` -- spending limits as a keyword | Runaway costs at 3 AM that nobody catches until the invoice |
| `taint user_input` -- taint tracking in the type system | Prompt injection (it becomes a **compile error**, not a production incident) |
| `secret string` -- compiler-enforced confidentiality | API keys leaked in logs, stdout, or error messages |
| Ownership checker + `defer` | Use-after-free, resource leaks, dangling connections |
| `health { ready, live }` -- K8s health probes as syntax | Silent failures where your agent is down and nobody knows |
| `metrics { counter, histogram }` -- Prometheus metrics as syntax | Blind spots in production where you can't see what's happening |
| `entropy_budget` + `invariant` -- statistical drift detection | Accuracy degrading from 90% to 60% while the agent keeps running |
| `supervisor { strategy: one_for_one }` -- Erlang/OTP fault tolerance | One agent crashing and taking everything down with it |
| `mesh` -- typed multi-agent pipelines | Type mismatches between pipeline stages discovered in production |
| Capability delegation + revocation | Sub-agents escalating privileges beyond what they were granted |
| Runtime capability fence | LLM hallucinating a tool call your agent shouldn't have access to |
| `result.confidence` on every `ask()` | Treating all LLM outputs as equally trustworthy |

## Self-Hosting

Limceron compiles itself. The Stage 1 self-hosted compiler is **8,755+ lines of Limceron** -- lexer, parser, type checker, and code generator.

The bootstrap chain is verified to a **fixed point**: Stage 0 (C99) compiles Stage 1 (Limceron). Stage 1 compiles itself, producing Stage 2. Stage 2 compiles itself, producing Stage 3. **Stages 2 and 3 produce identical output.** The compiler is stable.

```
Stage 0 (C99) --compiles--> Stage 1 (Limceron)
Stage 1       --compiles--> Stage 2
Stage 2       --compiles--> Stage 3
diff Stage2 Stage3 => identical (fixed point)
```

No other agent framework is self-hosting. This matters because it proves the language is expressive enough to build complex software -- not just toy agents.

```bash
make bootstrap    # Full Stage 0 -> 1 -> 2 -> 3 verification
```

## Native Compilation

Limceron compiles to native binaries via C99 transpilation. No interpreter. No VM. No runtime dependency.

```
.lceron / .lceron.md --> Lexer --> Parser --> TypeChecker --> Codegen (C99) --> gcc/clang --> Native Binary
```

Cross-compilation targets: **x86_64** and **aarch64**, Linux and macOS. The output is a single binary, typically ~1.2 MB, that you deploy with zero dependencies.

```bash
limceron build agent.lceron -o agent                              # native
limceron build agent.lceron -o agent --target aarch64-linux-gnu   # cross-compile
```

SSA intermediate representation is available for inspection:

```bash
limceron ir agent.lceron    # print SSA IR (debug)
```

## Getting Started

```bash
git clone https://github.com/limceron-lang/limceron
cd limceron
make stage0                 # requires only a C99 compiler (gcc or clang)
```

Run an example:

```bash
./build/limceron-stage0 run examples/language/01_hello_agent.lceron
```

Build a standalone binary:

```bash
./build/limceron-stage0 build examples/language/01_hello_agent.lceron -o my-agent
./my-agent
```

Scaffold a new project:

```bash
./build/limceron-stage0 init my-agent
# Creates my-agent/main.lceron + my-agent/main.lceron.md
```

Audit an agent before deploying it:

```bash
./build/limceron-stage0 audit examples/language/20_medical_categorizer.lceron
# Agents: 1 | LLM calls: 1 | Guards: 1 | Entropy score: 1.50 (review recommended)
```

## Tooling

| Tool | What it does | Why you want it |
|---|---|---|
| `limceron build` | Compile to native binary | Deploy a single file, no runtime |
| `limceron run` | Compile and execute | Fast iteration during development |
| `limceron emit` | Show generated C | Understand exactly what your agent compiles to |
| `limceron parse` | Show AST | Debug parser issues |
| `limceron ir` | Show SSA IR | Inspect optimization passes |
| `limceron audit` | Entropy/complexity analysis | Know your agent's risk profile before deploying |
| `limceron init` | Scaffold a new project | Start with the right structure |
| `limceron lsp` | Language Server Protocol | Autocomplete, diagnostics, go-to-definition in your editor |
| `limceron fmt` | Code formatter | Consistent style across your team |
| `limceron doctor` | Check runtime dependencies | Know if MySQL/Postgres/ONNX libs are available before you need them |
| `.lceron.md` | Markdown as source code | Near-zero learning curve for non-developers |

Error messages follow Rust's style -- source snippets with line numbers and carets pointing to the exact problem:

```
error: tainted input flows to LLM without sanitization
  --> agent.lceron:12:9
   |
12 |     ask(msg)
   |         ^^^ this value has taint @user_input
   |
   = hint: use sanitize(msg) to create an explicit trust boundary
```

## Test Suite

**477 tests. 2,123 assertions. All passing.**

The test suite covers the full pipeline: lexer, parser, type checker, code generator, runtime builtins, access control enforcement, taint propagation, secret type leakage prevention, capability delegation, generics monomorphization, SSA IR generation, and cross-compilation targeting.

```bash
make test    # run the full suite
```

## Competitive Landscape

| Capability | LangGraph | CrewAI | OpenAI SDK | Google ADK | **Limceron** |
|---|---|---|---|---|---|
| Compile-time access control | No | No | No | No | **Yes** |
| Taint tracking (prompt injection) | No | No | No | No | **Yes -- type error** |
| Budget as language primitive | No | No | No | No | **Yes -- keyword** |
| Entropy/confidence on every call | No | No | No | No | **Yes** |
| Statistical drift detection | No | No | No | No | **Yes -- auto-pause** |
| Native DB drivers (MySQL/Postgres) | No | No | No | No | **Yes -- direct TCP** |
| ONNX model binding | No | No | No | No | **Yes -- CPU, <5ms** |
| Compiles to native binary | No | No | No | No | **Yes -- ~1.2 MB** |
| Markdown as source code | No | No | No | No | **Yes** |
| Self-hosting compiler | No | No | No | No | **Yes** |
| Erlang/OTP supervisors | No | Basic retry | No | No | **Yes** |
| Capability delegation + revocation | No | No | Handoffs only | Hierarchy only | **Yes -- Hurd-inspired** |
| Secret type (leak prevention) | No | No | No | No | **Yes -- compile-time** |
| Runtime required | Python | Python | Python | Python/TS | **None** |

## Building from Source

```bash
git clone https://github.com/limceron-lang/limceron
cd limceron
make              # build the compiler (requires gcc or clang)
make test         # run all 477 tests
```

**Optional dependencies for production agents:**

```bash
brew install mysql-client       # for use driver("mysql")
brew install postgresql@16      # for use driver("postgres")
# libonnxruntime                # for use model("file.onnx") -- auto-detected
```

## License

Dual-licensed under [Apache 2.0](LICENSE-APACHE) and [MIT](LICENSE-MIT) — your choice.

**Your compiled programs are yours.** The output of the Limceron compiler (generated C, assembly, binaries) is not covered by these licenses. The runtime library linked into your programs is permissive (Apache 2.0 + MIT) and imposes zero copyleft obligations. See [NOTICE](NOTICE) for details.

## Etymology

**Limceron** /lim.ke.ron/ -- from Tolkien's Sindarin. **lim** (swift) + **ceron** (doer, agent). "Swift Agent."
