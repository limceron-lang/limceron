/*
 * Limceron Runtime — ONNX Model Binding
 *
 * Dual-path implementation:
 *   - When compiled with -DLCN_HAS_ONNXRUNTIME: real inference via ONNX Runtime C API
 *   - Otherwise: stub that logs calls and returns mock predictions
 *
 * The real path includes a minimal WordPiece tokenizer for BERT-style models,
 * softmax normalization, and Shannon entropy calculation.
 */

#include "onnx_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Real ONNX Runtime implementation
 * ================================================================ */
#ifdef LCN_HAS_ONNXRUNTIME

#include <onnxruntime_c_api.h>
#include <math.h>
#include <ctype.h>

/* --- Math utilities -------------------------------------------- */

static void lcn_softmax(float *logits, int n) {
    float max_val = logits[0];
    for (int i = 1; i < n; i++) {
        if (logits[i] > max_val) max_val = logits[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        logits[i] = expf(logits[i] - max_val);
        sum += logits[i];
    }
    for (int i = 0; i < n; i++) {
        logits[i] /= sum;
    }
}

static double lcn_shannon_entropy(float *probs, int n) {
    double H = 0.0;
    for (int i = 0; i < n; i++) {
        if (probs[i] > 1e-10f) {
            H -= (double)probs[i] * log2((double)probs[i]);
        }
    }
    return H;
}

/* --- Simple WordPiece tokenizer -------------------------------- */

#define WP_MAX_VOCAB    32000
#define WP_MAX_TOKEN_LEN  128
#define WP_MAX_TOKENS     512

typedef struct {
    char **tokens;          /* vocab[i] = token string */
    int    size;            /* number of tokens loaded */
    int    cls_id;          /* [CLS] token id */
    int    sep_id;          /* [SEP] token id */
    int    unk_id;          /* [UNK] token id */
    int    pad_id;          /* [PAD] token id */
} WordPieceVocab;

static WordPieceVocab *wp_load_vocab(const char *vocab_path) {
    FILE *f = fopen(vocab_path, "r");
    if (!f) {
        fprintf(stderr, "[model] warning: cannot open vocab file: %s\n", vocab_path);
        return NULL;
    }

    WordPieceVocab *v = (WordPieceVocab *)calloc(1, sizeof(WordPieceVocab));
    v->tokens = (char **)calloc(WP_MAX_VOCAB, sizeof(char *));
    v->cls_id = -1;
    v->sep_id = -1;
    v->unk_id = -1;
    v->pad_id = -1;

    char line[WP_MAX_TOKEN_LEN];
    while (fgets(line, sizeof(line), f) && v->size < WP_MAX_VOCAB) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        int id = v->size;
        v->tokens[id] = strdup(line);

        if (strcmp(line, "[CLS]") == 0) v->cls_id = id;
        else if (strcmp(line, "[SEP]") == 0) v->sep_id = id;
        else if (strcmp(line, "[UNK]") == 0) v->unk_id = id;
        else if (strcmp(line, "[PAD]") == 0) v->pad_id = id;

        v->size++;
    }
    fclose(f);

    if (v->unk_id < 0) v->unk_id = 0;
    fprintf(stderr, "[model] vocab loaded: %d tokens from %s\n", v->size, vocab_path);
    return v;
}

static void wp_free_vocab(WordPieceVocab *v) {
    if (!v) return;
    for (int i = 0; i < v->size; i++) {
        free(v->tokens[i]);
    }
    free(v->tokens);
    free(v);
}

/* Look up a token string in the vocab. Returns token id or -1. */
static int wp_lookup(WordPieceVocab *v, const char *token) {
    /* Linear scan — acceptable for 30k vocab at inference time.
     * A hash table would be better for high throughput. */
    for (int i = 0; i < v->size; i++) {
        if (strcmp(v->tokens[i], token) == 0) return i;
    }
    return -1;
}

/* Tokenize a single word using WordPiece (greedy longest-match).
 * Appends token IDs to ids[]. Returns updated count. */
static int wp_tokenize_word(WordPieceVocab *v, const char *word, int *ids, int count, int max) {
    size_t wlen = strlen(word);
    size_t start = 0;

    while (start < wlen && count < max) {
        size_t end = wlen;
        int found_id = -1;

        while (end > start) {
            char buf[WP_MAX_TOKEN_LEN];
            if (start > 0) {
                /* Subword: prepend ## */
                snprintf(buf, sizeof(buf), "##%.*s", (int)(end - start), word + start);
            } else {
                snprintf(buf, sizeof(buf), "%.*s", (int)(end - start), word + start);
            }

            int id = wp_lookup(v, buf);
            if (id >= 0) {
                found_id = id;
                break;
            }
            end--;
        }

        if (found_id >= 0) {
            ids[count++] = found_id;
            start = end;
        } else {
            /* Single character not in vocab — use [UNK] */
            ids[count++] = v->unk_id;
            start++;
        }
    }
    return count;
}

/* Full tokenization: lowercase, split on whitespace, WordPiece each word.
 * Returns number of tokens written to ids[]. */
static int wp_tokenize(WordPieceVocab *v, const char *text, int *ids, int max_ids) {
    if (!v || !text) return 0;

    int count = 0;

    /* [CLS] */
    if (v->cls_id >= 0 && count < max_ids) {
        ids[count++] = v->cls_id;
    }

    /* Lowercase copy */
    size_t len = strlen(text);
    char *lower = (char *)malloc(len + 1);
    for (size_t i = 0; i <= len; i++) {
        lower[i] = (char)tolower((unsigned char)text[i]);
    }

    /* Split on whitespace and tokenize each word */
    char *save = NULL;
    char *tok = strtok_r(lower, " \t\n\r", &save);
    while (tok && count < max_ids - 1) {  /* -1 to reserve room for [SEP] */
        count = wp_tokenize_word(v, tok, ids, count, max_ids - 1);
        tok = strtok_r(NULL, " \t\n\r", &save);
    }
    free(lower);

    /* [SEP] */
    if (v->sep_id >= 0 && count < max_ids) {
        ids[count++] = v->sep_id;
    }

    return count;
}

/* --- Label map ------------------------------------------------- */

#define MAX_LABELS 256

typedef struct {
    char *names[MAX_LABELS];
    int   count;
} LabelMap;

static LabelMap *load_label_map(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[model] warning: cannot open label map: %s\n", path);
        return NULL;
    }

    LabelMap *lm = (LabelMap *)calloc(1, sizeof(LabelMap));
    char line[256];
    while (fgets(line, sizeof(line), f) && lm->count < MAX_LABELS) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        lm->names[lm->count++] = strdup(line);
    }
    fclose(f);
    fprintf(stderr, "[model] label map loaded: %d labels from %s\n", lm->count, path);
    return lm;
}

static void free_label_map(LabelMap *lm) {
    if (!lm) return;
    for (int i = 0; i < lm->count; i++) free(lm->names[i]);
    free(lm);
}

/* --- LcnModel with real ORT session ---------------------------- */

struct LcnModel {
    char           *model_path;
    char           *label_map_path;
    bool            loaded;
    const OrtApi   *ort;
    OrtEnv         *env;
    OrtSession     *session;
    OrtSessionOptions *session_opts;
    OrtMemoryInfo  *mem_info;
    WordPieceVocab *vocab;
    LabelMap       *labels;
    int             num_outputs;   /* number of output classes (detected at load) */
};

/* Derive vocab.txt path from model path (same directory). */
static char *derive_vocab_path(const char *model_path) {
    const char *last_slash = strrchr(model_path, '/');
    size_t dir_len;
    if (last_slash) {
        dir_len = (size_t)(last_slash - model_path + 1);
    } else {
        dir_len = 0;
    }
    const char *vocab_name = "vocab.txt";
    char *path = (char *)malloc(dir_len + strlen(vocab_name) + 1);
    if (dir_len > 0) memcpy(path, model_path, dir_len);
    strcpy(path + dir_len, vocab_name);
    return path;
}

LcnModel *lcn_model_load(const char *model_path, const char *label_map_path) {
    LcnModel *m = (LcnModel *)calloc(1, sizeof(LcnModel));
    if (!m) return NULL;
    m->model_path = model_path ? strdup(model_path) : NULL;
    m->label_map_path = label_map_path ? strdup(label_map_path) : NULL;

    /* Initialize ONNX Runtime */
    m->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!m->ort) {
        fprintf(stderr, "[model] error: failed to get ONNX Runtime API\n");
        m->loaded = false;
        return m;
    }

    OrtStatus *status = NULL;

    /* Create environment */
    status = m->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "limceron", &m->env);
    if (status) {
        fprintf(stderr, "[model] error: ORT CreateEnv: %s\n", m->ort->GetErrorMessage(status));
        m->ort->ReleaseStatus(status);
        m->loaded = false;
        return m;
    }

    /* Create session options */
    status = m->ort->CreateSessionOptions(&m->session_opts);
    if (status) {
        fprintf(stderr, "[model] error: ORT CreateSessionOptions: %s\n", m->ort->GetErrorMessage(status));
        m->ort->ReleaseStatus(status);
        m->loaded = false;
        return m;
    }
    m->ort->SetIntraOpNumThreads(m->session_opts, 1);
    m->ort->SetSessionGraphOptimizationLevel(m->session_opts, ORT_ENABLE_BASIC);

    /* Create session (load the ONNX model) */
    status = m->ort->CreateSession(m->env, model_path, m->session_opts, &m->session);
    if (status) {
        fprintf(stderr, "[model] error: ORT CreateSession(%s): %s\n",
                model_path ? model_path : "?", m->ort->GetErrorMessage(status));
        m->ort->ReleaseStatus(status);
        m->loaded = false;
        return m;
    }

    /* Create memory info for tensor allocation */
    status = m->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->mem_info);
    if (status) {
        fprintf(stderr, "[model] error: ORT CreateCpuMemoryInfo: %s\n", m->ort->GetErrorMessage(status));
        m->ort->ReleaseStatus(status);
        m->loaded = false;
        return m;
    }

    /* Load vocab from same directory as model */
    char *vocab_path = derive_vocab_path(model_path);
    m->vocab = wp_load_vocab(vocab_path);
    free(vocab_path);

    /* Load label map */
    if (label_map_path) {
        m->labels = load_label_map(label_map_path);
    }

    /* Detect number of output classes by inspecting output tensor shape */
    m->num_outputs = 2;  /* default for binary classification */
    {
        OrtTypeInfo *out_type_info = NULL;
        status = m->ort->SessionGetOutputTypeInfo(m->session, 0, &out_type_info);
        if (!status && out_type_info) {
            const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
            m->ort->CastTypeInfoToTensorInfo(out_type_info, &tensor_info);
            if (tensor_info) {
                size_t dim_count = 0;
                m->ort->GetDimensionsCount(tensor_info, &dim_count);
                if (dim_count >= 2) {
                    int64_t *dims = (int64_t *)malloc(dim_count * sizeof(int64_t));
                    m->ort->GetDimensions(tensor_info, dims, dim_count);
                    if (dims[dim_count - 1] > 0) {
                        m->num_outputs = (int)dims[dim_count - 1];
                    }
                    free(dims);
                }
            }
            m->ort->ReleaseTypeInfo(out_type_info);
        } else if (status) {
            m->ort->ReleaseStatus(status);
        }
    }

    if (m->labels && m->labels->count > 0) {
        m->num_outputs = m->labels->count;
    }

    m->loaded = true;
    fprintf(stderr, "[model] loaded: %s (%d classes, ONNX Runtime)\n",
            model_path ? model_path : "?", m->num_outputs);
    return m;
}

LcnModelResult lcn_model_predict(LcnModel *model, const char *text) {
    LcnModelResult r;
    memset(&r, 0, sizeof(r));

    if (!model || !model->loaded || !model->session) {
        r.ok = false;
        r.error = "model not loaded";
        return r;
    }

    if (!model->vocab) {
        r.ok = false;
        r.error = "no vocab.txt — cannot tokenize input";
        return r;
    }

    const OrtApi *ort = model->ort;
    OrtStatus *status = NULL;

    /* 1. Tokenize */
    int token_ids[WP_MAX_TOKENS];
    int num_tokens = wp_tokenize(model->vocab, text, token_ids, WP_MAX_TOKENS);

    if (num_tokens == 0) {
        r.ok = false;
        r.error = "tokenization produced no tokens";
        return r;
    }

    /* 2. Build input tensors */
    int64_t shape[2] = {1, (int64_t)num_tokens};
    size_t tensor_size = (size_t)num_tokens * sizeof(int64_t);

    int64_t *input_ids      = (int64_t *)calloc((size_t)num_tokens, sizeof(int64_t));
    int64_t *attention_mask  = (int64_t *)calloc((size_t)num_tokens, sizeof(int64_t));
    int64_t *token_type_ids  = (int64_t *)calloc((size_t)num_tokens, sizeof(int64_t));

    for (int i = 0; i < num_tokens; i++) {
        input_ids[i] = (int64_t)token_ids[i];
        attention_mask[i] = 1;
        token_type_ids[i] = 0;
    }

    OrtValue *input_tensors[3] = {NULL, NULL, NULL};

    status = ort->CreateTensorWithDataAsOrtValue(
        model->mem_info, input_ids, tensor_size,
        shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[0]);
    if (status) {
        fprintf(stderr, "[model] error: CreateTensor(input_ids): %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        goto cleanup;
    }

    status = ort->CreateTensorWithDataAsOrtValue(
        model->mem_info, attention_mask, tensor_size,
        shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[1]);
    if (status) {
        fprintf(stderr, "[model] error: CreateTensor(attention_mask): %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        goto cleanup;
    }

    status = ort->CreateTensorWithDataAsOrtValue(
        model->mem_info, token_type_ids, tensor_size,
        shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensors[2]);
    if (status) {
        fprintf(stderr, "[model] error: CreateTensor(token_type_ids): %s\n", ort->GetErrorMessage(status));
        ort->ReleaseStatus(status);
        goto cleanup;
    }

    /* 3. Run inference */
    const char *input_names[]  = {"input_ids", "attention_mask", "token_type_ids"};
    const char *output_names[] = {"logits"};

    /* Determine how many inputs the model actually expects */
    size_t model_num_inputs = 0;
    ort->SessionGetInputCount(model->session, &model_num_inputs);
    if (model_num_inputs > 3) model_num_inputs = 3;

    /* Determine the actual output name from the model */
    size_t model_num_outputs = 0;
    ort->SessionGetOutputCount(model->session, &model_num_outputs);

    OrtAllocator *allocator = NULL;
    ort->GetAllocatorWithDefaultOptions(&allocator);

    char *actual_output_name = NULL;
    if (model_num_outputs > 0) {
        ort->SessionGetOutputName(model->session, 0, allocator, &actual_output_name);
    }
    const char *out_names[1];
    out_names[0] = actual_output_name ? actual_output_name : output_names[0];

    OrtValue *output_tensor = NULL;
    status = ort->Run(model->session, NULL,
                      input_names, (const OrtValue *const *)input_tensors,
                      model_num_inputs,
                      out_names, 1, &output_tensor);

    if (actual_output_name) {
        allocator->Free(allocator, actual_output_name);
    }

    if (status) {
        fprintf(stderr, "[model] error: ORT Run: %s\n", ort->GetErrorMessage(status));
        r.ok = false;
        r.error = "inference failed";
        ort->ReleaseStatus(status);
        goto cleanup;
    }

    /* 4. Extract logits */
    float *logits = NULL;
    ort->GetTensorMutableData(output_tensor, (void **)&logits);
    if (!logits) {
        r.ok = false;
        r.error = "failed to get output tensor data";
        goto cleanup;
    }

    int num_classes = model->num_outputs;

    /* Copy logits so softmax can modify in-place */
    float *probs = (float *)malloc((size_t)num_classes * sizeof(float));
    memcpy(probs, logits, (size_t)num_classes * sizeof(float));

    /* 5. Softmax */
    lcn_softmax(probs, num_classes);

    /* 6. Find argmax */
    int best_id = 0;
    float best_prob = probs[0];
    for (int i = 1; i < num_classes; i++) {
        if (probs[i] > best_prob) {
            best_prob = probs[i];
            best_id = i;
        }
    }

    /* 7. Shannon entropy + confidence */
    double H = lcn_shannon_entropy(probs, num_classes);
    double max_entropy = log2((double)num_classes);
    double confidence = (max_entropy > 0.0) ? (1.0 - H / max_entropy) : 1.0;

    /* 8. Build result — confidence as percentage (0-100) for int64_t compatibility */
    r.ok = true;
    r.class_id = best_id;
    r.confidence = confidence * 100.0;
    r.entropy = H;

    /* Label lookup */
    if (model->labels && best_id < model->labels->count) {
        r.label = model->labels->names[best_id];
    } else {
        /* Return a static buffer with class id */
        static char label_buf[32];
        snprintf(label_buf, sizeof(label_buf), "CLASS_%d", best_id);
        r.label = label_buf;
    }

    free(probs);
    if (output_tensor) ort->ReleaseValue(output_tensor);
    free(input_ids);
    free(attention_mask);
    free(token_type_ids);
    for (int i = 0; i < 3; i++) {
        if (input_tensors[i]) ort->ReleaseValue(input_tensors[i]);
    }
    return r;

cleanup:
    if (!r.ok && !r.error) {
        r.ok = false;
        r.error = "inference error";
    }
    if (output_tensor) ort->ReleaseValue(output_tensor);
    free(input_ids);
    free(attention_mask);
    free(token_type_ids);
    for (int i = 0; i < 3; i++) {
        if (input_tensors[i]) ort->ReleaseValue(input_tensors[i]);
    }
    return r;
}

void lcn_model_free(LcnModel *model) {
    if (!model) return;
    if (model->ort) {
        if (model->session)      model->ort->ReleaseSession(model->session);
        if (model->session_opts) model->ort->ReleaseSessionOptions(model->session_opts);
        if (model->mem_info)     model->ort->ReleaseMemoryInfo(model->mem_info);
        if (model->env)          model->ort->ReleaseEnv(model->env);
    }
    wp_free_vocab(model->vocab);
    free_label_map(model->labels);
    free(model->model_path);
    free(model->label_map_path);
    free(model);
}

const char *lcn_model_info(LcnModel *model) {
    if (!model) return "null model";
    if (!model->loaded) return "model not loaded";
    return model->model_path ? model->model_path : "unknown";
}

/* ================================================================
 * Stub implementation (no ONNX Runtime available)
 * ================================================================ */
#else  /* !LCN_HAS_ONNXRUNTIME */

struct LcnModel {
    char *model_path;
    char *label_map_path;
    bool  loaded;
};

LcnModel *lcn_model_load(const char *model_path, const char *label_map_path) {
    LcnModel *m = (LcnModel *)calloc(1, sizeof(LcnModel));
    if (!m) return NULL;
    m->model_path = model_path ? strdup(model_path) : NULL;
    m->label_map_path = label_map_path ? strdup(label_map_path) : NULL;
    m->loaded = true;
    fprintf(stderr, "[model] loaded: %s (STUB — install libonnxruntime for real inference)\n",
            model_path ? model_path : "?");
    return m;
}

LcnModelResult lcn_model_predict(LcnModel *model, const char *text) {
    LcnModelResult r;
    memset(&r, 0, sizeof(r));
    if (!model || !model->loaded) {
        r.ok = false;
        r.error = "model not loaded";
        return r;
    }
    /* STUB: return a mock prediction with plausible entropy values */
    r.ok = true;
    r.label = "STUB_PREDICTION";
    r.confidence = 0.99;
    r.entropy = 0.01;
    r.class_id = 0;
    fprintf(stderr, "[model] predict (STUB): \"%s\" -> %s (conf=%.2f, H=%.4f)\n",
            text ? text : "", r.label, r.confidence, r.entropy);
    return r;
}

void lcn_model_free(LcnModel *model) {
    if (!model) return;
    free(model->model_path);
    free(model->label_map_path);
    free(model);
}

const char *lcn_model_info(LcnModel *model) {
    if (!model) return "null model";
    return model->model_path ? model->model_path : "unknown";
}

#endif /* LCN_HAS_ONNXRUNTIME */
