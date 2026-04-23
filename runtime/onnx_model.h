/*
 * Limceron Runtime — ONNX Model Binding
 *
 * Provides local model inference for fine-tuned BERT models (e.g., Patana).
 * Syntax: use model("path.onnx") as classifier
 *         classifier.predict(text) -> LcnModelResult
 *
 * Dual-path:
 *   - Compiled with -DLCN_HAS_ONNXRUNTIME: real inference via ONNX Runtime C API
 *   - Without: stub returning mock predictions (always compiles)
 *
 * When using real inference, the model directory should contain:
 *   - model.onnx     — the ONNX model file
 *   - vocab.txt      — WordPiece vocabulary (one token per line, ~30k entries)
 *   - labels.txt     — class labels (one per line, optional)
 */

#ifndef LCN_ONNX_MODEL_H
#define LCN_ONNX_MODEL_H

#include <stdbool.h>

/* Opaque model handle */
typedef struct LcnModel LcnModel;

/* Model prediction result — same interface as LcnLlmOutput for interop */
typedef struct {
    const char *label;       /* predicted class label */
    double      confidence;  /* normalized confidence [0, 1]: 1.0 - H/log2(num_classes) */
    double      entropy;     /* Shannon entropy of softmax distribution (bits) */
    int         class_id;    /* numeric class index (argmax of softmax) */
    bool        ok;
    const char *error;
} LcnModelResult;

/* Load an ONNX model from file path.
 * label_map_path: optional text file mapping class IDs to label strings (one per line).
 * Returns NULL only on allocation failure; check model->loaded for load status. */
LcnModel *lcn_model_load(const char *model_path, const char *label_map_path);

/* Run prediction on text input. Returns result with label + confidence + entropy.
 * The model handles tokenization internally (BERT WordPiece via vocab.txt). */
LcnModelResult lcn_model_predict(LcnModel *model, const char *text);

/* Free model resources (ORT session, vocab, label map). */
void lcn_model_free(LcnModel *model);

/* Get model info string (for diagnostics). */
const char *lcn_model_info(LcnModel *model);

#endif
