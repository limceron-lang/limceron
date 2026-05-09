/*
 * Limceron Compiler — WIT (WebAssembly Interface Types) Emitter
 *
 * Emits a `.wit` file describing the agent's imports/exports derived from
 * its `capabilities: [...]` declaration and exported functions.
 *
 * Pairs with the `.wasm` artifact produced by ir_emit_wasm.c.  The WIT file
 * is the declarative manifest a host runtime uses to wire up capability
 * implementations to the WebAssembly Component Model interface.
 */

#ifndef LCN_WIT_EMIT_H
#define LCN_WIT_EMIT_H

#include "lcn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Emit a WIT package describing the agent(s) in `program` into `wit_path`.
 *
 * Walks the top-level AST for AST_AGENT nodes, collects each agent's
 * declared capabilities (`capabilities: [llm.classify, ...]`) and exported
 * functions, and writes a syntactically valid WIT document to `wit_path`.
 *
 * Caller is responsible for choosing the path (typically: replace the
 * `.wasm` extension on the build output with `.wit`).
 *
 * Returns 0 on success, 1 on error (file open failure, null arguments,
 * or no agents found in `program`).
 */
int lcn_emit_wit(AstNode *program, const char *wit_path);

#ifdef __cplusplus
}
#endif

#endif /* LCN_WIT_EMIT_H */
