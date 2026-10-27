// AAC-Decoder für Resolve unter Linux.
//
// Resolve bringt FFmpeg 6.0 mit (libavcodec.so.60), gebaut mit --disable-decoder='aac*' und
// --disable-parser='aac*'; Audio in MP4/MOV/M4A mit AAC bleibt darum stumm. Die Bibliothek ist
// außerdem von Blackmagic erweitert (AVS3-Audio, von Resolves libavformat benutzt), lässt sich
// also nicht gegen ein eigenes FFmpeg 6.0 tauschen.
//
// Stattdessen lädt der Lader eine zweite, schlanke libavcodec 6.0 mit nur dem AAC-Decoder
// (make aac, liegt in ffmpeg-aac/ neben libresolve-linux-vst.so) mit RTLD_LOCAL: Sie nutzt Resolves
// libavutil.so.58, ihre Symbole bleiben unsichtbar (FFmpeg linkt mit -Bsymbolic). Die
// Funktionen hier ersetzen avcodec_find_decoder, avcodec_find_decoder_by_name und
// av_parser_init (Resolve und libavformat rufen sie über die PLT): Findet Resolves libavcodec
// nichts und geht es um AAC, kommt der Decoder bzw. Parser aus der zweiten Bibliothek.
// Resolves libavcodec betreibt ihn dann wie einen eigenen; beide sind dieselbe Version, die
// internen Strukturen passen.
//
// Für MP4/MOV fragt Resolve FFmpeg aber gar nicht erst nach AAC: Sein QuickTime-Leser (der
// aus mp4a den FourCC 'aac ' macht) schickt nur bestimmte FourCCs an die FFmpeg-Decoderklasse
// und schlägt dort den Codec in einer Tabelle nach (alac, flac, .mp3, ac-3). Darum passt der
// Lader in Resolve drei Stellen im Speicher an (patchResolve, beim Start von main,
// nachdem Resolve die Tabelle angelegt hat): 'aac ' und mp4a → AAC in die Tabelle, beide in
// der Weiche auf den Weg von .mp3 und flac, und beim Öffnen die AudioSpecificConfig aus der
// Spurbeschreibung als extradata (sonst kennt der Decoder die Kanäle nicht). Die Stellen findet
// er über Byte-Muster (sites.cpp); fehlt eine (andere Version), ändert er nichts und schreibt
// das ins Log.

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "sites.h"
#include "vst2.h"

#define EXPORT extern "C" __attribute__((visibility("default"), used))

namespace {

// enum AVCodecID aus FFmpeg 6.0 (libavcodec/codec_id.h)
constexpr int kCodecAac = 0x15002;
constexpr int kCodecAacLatm = 0x15031;
constexpr int kInputPadding = 64; // AV_INPUT_BUFFER_PADDING_SIZE

bool isAac(int id)
{
    return id == kCodecAac || id == kCodecAacLatm;
}

bool isAacName(const char *name)
{
    return name && (strcmp(name, "aac") == 0 || strcmp(name, "aac_fixed") == 0 || strcmp(name, "aac_latm") == 0);
}

// Resolves Funktion. Nur Version 60: Andere Programme, die LD_PRELOAD erben und ein anderes
// FFmpeg benutzen, bekommen ihre eigene Funktion und keinen AAC-Decoder von hier.
template <typename F>
F realFunction(const char *name, bool *isResolve)
{
    if (void *f = dlvsym(RTLD_NEXT, name, "LIBAVCODEC_60")) {
        *isResolve = true;
        return reinterpret_cast<F>(f);
    }
    *isResolve = false;
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

std::string ownDirectory()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&ownDirectory), &info) || !info.dli_fname)
        return {};
    std::string path = info.dli_fname;
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Die zweite libavcodec, einmal geladen. RESOLVE_LINUX_VST_AAC: Pfad zur Bibliothek, 0 = aus.
void *aacLibrary()
{
    static void *handle = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string path;
        if (const char *env = getenv("RESOLVE_LINUX_VST_AAC"); env && *env) {
            if (strcmp(env, "0") == 0) {
                logMessage("AAC: abgeschaltet (RESOLVE_LINUX_VST_AAC=0)");
                return;
            }
            path = env;
        } else {
            path = ownDirectory() + "/ffmpeg-aac/libavcodec.so.60";
        }
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle)
            logMessage("AAC: Decoder aus %s", path.c_str());
        else
            logMessage("AAC: kein Decoder (%s; make aac?)", dlerror());
    });
    return handle;
}

template <typename F>
F aacFunction(const char *name)
{
    void *lib = aacLibrary();
    return lib ? reinterpret_cast<F>(dlsym(lib, name)) : nullptr;
}

} // namespace

EXPORT const void *avcodec_find_decoder(int id)
{
    using F = const void *(*)(int);
    static bool resolve = false;
    static F real = realFunction<F>("avcodec_find_decoder", &resolve);
    const void *codec = real ? real(id) : nullptr;
    if (!codec && resolve && isAac(id))
        if (static F own = aacFunction<F>("avcodec_find_decoder"); own)
            codec = own(id);
    return codec;
}

EXPORT const void *avcodec_find_decoder_by_name(const char *name)
{
    using F = const void *(*)(const char *);
    static bool resolve = false;
    static F real = realFunction<F>("avcodec_find_decoder_by_name", &resolve);
    const void *codec = real ? real(name) : nullptr;
    if (!codec && resolve && isAacName(name))
        if (static F own = aacFunction<F>("avcodec_find_decoder_by_name"); own)
            codec = own(name);
    return codec;
}

// Der Parser-Kontext kommt ganz aus der zweiten Bibliothek; av_parser_parse2 und
// av_parser_close in Resolves libavcodec rufen nur seine Funktionen auf und geben den
// Speicher mit av_free frei (libavutil, für beide dieselbe).
EXPORT void *av_parser_init(int id)
{
    using F = void *(*)(int);
    static bool resolve = false;
    static F real = realFunction<F>("av_parser_init", &resolve);
    void *parser = real ? real(id) : nullptr;
    if (!parser && resolve && isAac(id))
        if (static F own = aacFunction<F>("av_parser_init"); own)
            parser = own(id);
    return parser;
}

// Rendern als MP4/MOV/MKV mit AAC aus einem Encoder-Plugin (IO-Plugin, z. B. FDK-AAC): Resolve
// schreibt über seine libavformat 60, gibt der Spur aber keine extradata mit, obwohl das Plugin
// die AudioSpecificConfig als Magic Cookie liefert. Der mov-Muxer schreibt dann ein esds ohne
// DecoderSpecificInfo; ffprobe zeigt profile=-1, Browser spielen den Ton nicht ab (ihnen fehlt
// mp4a.40.2). Darum vor avformat_write_header: AAC-Spuren ohne extradata bekommen die
// AudioSpecificConfig für AAC-LC aus Abtastrate und Kanälen (wie sie FDK-AAC selbst liefert).
namespace {

// AVFormatContext, AVStream, AVCodecParameters in FFmpeg 6.0 (libavformat 60)
constexpr size_t kFormatNbStreams = 0x2c;
constexpr size_t kFormatStreams = 0x30;
constexpr size_t kStreamCodecpar = 0x10;
constexpr size_t kParType = 0x00;
constexpr size_t kParCodecId = 0x04;
constexpr size_t kParExtradata = 0x10;
constexpr size_t kParExtradataSize = 0x18;
constexpr size_t kParProfile = 0x30;
constexpr size_t kParChannels = 0x70; // veraltet, Ersatz für ch_layout
constexpr size_t kParSampleRate = 0x74;
constexpr size_t kParChLayoutChannels = 0x94;
constexpr int kMediaAudio = 1;
constexpr int kProfileUnknown = -99;
constexpr int kProfileAacLow = 1;

bool isResolveExecutable();

template <typename T>
T &field(void *base, size_t offset)
{
    return *reinterpret_cast<T *>(static_cast<uint8_t *>(base) + offset);
}

// AudioSpecificConfig für AAC-LC (ISO 14496-3, 1.6.2.1): 5 Bit Objekttyp 2, 4 Bit Index der
// Abtastrate, 4 Bit Kanalkonfiguration, 3 Bit 0.
bool aacLcConfig(int sampleRate, int channels, uint8_t out[2])
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int rateIndex = -1;
    for (int i = 0; i < int(sizeof rates / sizeof *rates); ++i)
        if (rates[i] == sampleRate)
            rateIndex = i;
    const int config = channels >= 1 && channels <= 6 ? channels : channels == 8 ? 7 : -1;
    if (rateIndex < 0 || config < 0)
        return false;
    out[0] = uint8_t(2 << 3 | rateIndex >> 1);
    out[1] = uint8_t((rateIndex & 1) << 7 | config << 3);
    return true;
}

void addAacConfig(void *format)
{
    const unsigned n = field<unsigned>(format, kFormatNbStreams);
    void **streams = field<void **>(format, kFormatStreams);
    for (unsigned i = 0; i < n && streams; ++i) {
        void *par = field<void *>(streams[i], kStreamCodecpar);
        if (!par || field<int>(par, kParType) != kMediaAudio || field<int>(par, kParCodecId) != kCodecAac
            || field<int>(par, kParExtradataSize) > 0)
            continue;
        const int profile = field<int>(par, kParProfile);
        const int rate = field<int>(par, kParSampleRate);
        int channels = field<int>(par, kParChLayoutChannels);
        if (channels <= 0)
            channels = field<int>(par, kParChannels);
        uint8_t asc[2];
        if ((profile != kProfileUnknown && profile != kProfileAacLow) || !aacLcConfig(rate, channels, asc)) {
            logMessage("AAC: Spur %u ohne AudioSpecificConfig, nicht ergänzt (Profil %d, %d Hz, %d Kanäle)", i,
                       profile, rate, channels);
            continue;
        }
        using Alloc = void *(*)(size_t);
        static auto avMallocz = reinterpret_cast<Alloc>(dlsym(RTLD_DEFAULT, "av_mallocz"));
        void *p = avMallocz ? avMallocz(sizeof asc + kInputPadding) : nullptr;
        if (!p)
            continue;
        memcpy(p, asc, sizeof asc);
        field<void *>(par, kParExtradata) = p;
        field<int>(par, kParExtradataSize) = sizeof asc;
        field<int>(par, kParProfile) = kProfileAacLow;
        logMessage("AAC: Spur %u ohne AudioSpecificConfig, ergänzt: %02x%02x (AAC-LC, %d Hz, %d Kanäle)", i, asc[0],
                   asc[1], rate, channels);
    }
}

} // namespace

EXPORT int avformat_write_header(void *format, void **options)
{
    using F = int (*)(void *, void **);
    static auto real = reinterpret_cast<F>(dlvsym(RTLD_NEXT, "avformat_write_header", "LIBAVFORMAT_60"));
    if (!real) { // anderes FFmpeg: Aufbau unbekannt, nichts anfassen
        static std::once_flag logged;
        if (isResolveExecutable())
            std::call_once(logged, [] {
                logMessage("AAC: libavformat ist nicht Version 60, AudioSpecificConfig beim Rendern nicht ergänzt; resolve-linux-vst check");
            });
        return reinterpret_cast<F>(dlsym(RTLD_NEXT, "avformat_write_header"))(format, options);
    }
    if (format)
        addAacConfig(format);
    return real(format, options);
}

// Anpassungen in Resolve (Executable ohne PIE). Die Stellen sucht sites.cpp über Byte-Muster
// (Adressen hier aus 21.1.1); `resolve-linux-vst check` prüft sie ohne Resolve zu starten.
//
// Weiche (Spur eines QuickTime-Clips → Decoder): Vergleiche auf den FourCC in r12d; was
// keinen eigenen Zweig hat ('aac ', mp4a), landet im allgemeinen Zweig, der die Quelle holt
// (rbx, rax = rbx == 0) und nur alac nimmt (0x5deaedd), sonst verwirft (0x5deaf36). Ersatz
// ab 0x5deaecf: dasselbe, dazu 'aac ' und mp4a → 0x5deaf15, der Zweig von .mp3 und flac
// (FFmpeg-Decoderklasse) nach dem Holen der Quelle.
//
// Decoderklasse: Nach avcodec_alloc_context3 holt sie für ALAC und FLAC die Zusatzdaten der
// Spur (virtuelle Funktion 0x50 der Quelle, std::vector<uint8_t>) als extradata, für andere
// Codecs nicht. Ersatz ab 0x5ddfdeb: dieselben Vergleiche, für AAC vorher
// resolveVstAacPrepare (rbx = Decoder, rax = AVCodecContext; rax bleibt erhalten).
//
// Resolve erwartet zu jedem Paket einen Frame (wie von den Decodern des Systems unter Windows
// und macOS) und wertet EAGAIN als Fehler. FFmpeg verwirft aber Vorlauf-Frames (skip_samples,
// etwa 1024 Samples bei Dateien von libfaac). Mit AV_CODEC_FLAG2_SKIP_MANUAL gibt es jeden
// Frame aus und meldet den Vorlauf nur als Zusatzdaten.
//
// Tabelle FourCC → AVCodecID: std::map<uint32_t, int> (libc++, 0x297ed7f0), angelegt beim
// Start von Resolve.

extern "C" {
__attribute__((visibility("hidden"))) uintptr_t resolveVstSwitchTargets[3];
__attribute__((visibility("hidden"))) uintptr_t resolveVstCodecTargets[2];
__attribute__((visibility("hidden"))) void resolveVstAacPrepare(void *decoder, void *context);
void resolveVstSwitch();
void resolveVstCodec();
}

asm(R"(
    .intel_syntax noprefix
    .text
    .p2align 4
    .type resolveVstSwitch, @function
resolveVstSwitch:
    test rbx, rbx                           # keine Quelle
    jz 1f
    cmp r12d, 0x616c6163                    # 'alac'
    jne 2f
    jmp qword ptr [rip + resolveVstSwitchTargets]
2:  cmp r12d, 0x61616320                    # 'aac ': wie '.mp3' und 'flac'
    je 3f
    cmp r12d, 0x6d703461                    # 'mp4a'
    jne 1f
3:  jmp qword ptr [rip + resolveVstSwitchTargets + 8]
1:  jmp qword ptr [rip + resolveVstSwitchTargets + 16]
    .size resolveVstSwitch, . - resolveVstSwitch

    .p2align 4
    .type resolveVstCodec, @function
resolveVstCodec:
    mov ecx, dword ptr [r13 + 0x20]         # AVCodecID aus der Tabelle
    cmp ecx, 0x15010                        # ALAC
    je 2f
    cmp ecx, 0x1500c                        # FLAC
    je 2f
    cmp ecx, 0x15002                        # AAC
    jne 1f
    push rax
    sub rsp, 8
    mov rdi, rbx
    mov rsi, rax
    call resolveVstAacPrepare
    add rsp, 8
    pop rax
1:  jmp qword ptr [rip + resolveVstCodecTargets]
2:  jmp qword ptr [rip + resolveVstCodecTargets + 8]
    .size resolveVstCodec, . - resolveVstCodec
    .att_syntax prefix
)");

namespace {

constexpr uint32_t kFourccAac = 0x61616320; // 'aac '
constexpr uint32_t kFourccMp4a = 0x6d703461;

// AVCodecContext in FFmpeg 6.0
constexpr size_t kContextFlags2 = 0x50;
constexpr size_t kContextExtradata = 0x58;
constexpr size_t kContextExtradataSize = 0x60;
constexpr int kFlag2SkipManual = 1 << 29; // AV_CODEC_FLAG2_SKIP_MANUAL

struct ByteVector { // libc++ std::vector<uint8_t>
    uint8_t *begin, *end, *capacity;
};

std::string hex(const uint8_t *p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < 48; ++i) {
        snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return n > 48 ? s + "…" : s;
}

// Länge eines MPEG-4-Deskriptors (bis zu vier Bytes, je sieben Bit).
bool descriptorLength(const uint8_t *&p, const uint8_t *end, size_t *len)
{
    *len = 0;
    for (int i = 0; i < 4; ++i) {
        if (p >= end)
            return false;
        uint8_t b = *p++;
        *len = (*len << 7) | (b & 0x7f);
        if (!(b & 0x80))
            return *len <= size_t(end - p);
    }
    return false;
}

// Sucht in ES_Descriptor (0x03) → DecoderConfigDescriptor (0x04) die DecoderSpecificInfo
// (0x05), die AudioSpecificConfig.
bool findSpecificInfo(const uint8_t *p, const uint8_t *end, const uint8_t **info, size_t *infoLen, int *objectType)
{
    while (p < end) {
        uint8_t tag = *p++;
        size_t len;
        if (!descriptorLength(p, end, &len))
            return false;
        const uint8_t *body = p, *bodyEnd = p + len;
        if (tag == 0x03 && len >= 3) {
            uint8_t flags = body[2];
            body += 3;
            if (flags & 0x80)
                body += 2;
            if ((flags & 0x40) && body < bodyEnd)
                body += 1 + *body;
            if (flags & 0x20)
                body += 2;
            if (body <= bodyEnd && findSpecificInfo(body, bodyEnd, info, infoLen, objectType))
                return true;
        } else if (tag == 0x04 && len >= 13) {
            *objectType = body[0];
            if (findSpecificInfo(body + 13, bodyEnd, info, infoLen, objectType))
                return true;
        } else if (tag == 0x05) {
            *info = body;
            *infoLen = len;
            return true;
        }
        p = bodyEnd;
    }
    return false;
}

// AudioSpecificConfig aus den Zusatzdaten der Spur: ein esds-Atom (mit oder ohne Kopf) oder
// schon die AudioSpecificConfig selbst.
bool audioSpecificConfig(const uint8_t *p, size_t n, const uint8_t **asc, size_t *ascLen, int *objectType)
{
    if (n >= 12 && memcmp(p + 4, "esds", 4) == 0)
        return findSpecificInfo(p + 12, p + n, asc, ascLen, objectType);
    if (n >= 5 && p[0] == 0 && p[4] == 0x03)
        return findSpecificInfo(p + 4, p + n, asc, ascLen, objectType);
    if (n >= 1 && p[0] == 0x03)
        return findSpecificInfo(p, p + n, asc, ascLen, objectType);
    if (n >= 2 && n <= 64) {
        *asc = p;
        *ascLen = n;
        return true;
    }
    return false;
}

// Überschreibt `len` Bytes ab `addr` mit `jmp [rip+0]; dq dest` (14 Byte), Rest int3.
bool writeJump(uintptr_t addr, size_t len, void *dest)
{
    uint8_t code[32];
    if (len < 14 || len > sizeof code)
        return false;
    memset(code, 0xcc, sizeof code);
    const uint8_t jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(code, jump, sizeof jump);
    uintptr_t d = reinterpret_cast<uintptr_t>(dest);
    memcpy(code + 6, &d, sizeof d);

    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~uintptr_t(pageSize - 1);
    size_t span = addr + len - page;
    if (mprotect(reinterpret_cast<void *>(page), span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    memcpy(reinterpret_cast<void *>(addr), code, len);
    mprotect(reinterpret_cast<void *>(page), span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(addr), reinterpret_cast<char *>(addr + len));
    return true;
}

// libc++ __tree_node<pair<const uint32_t, int>> und std::map: begin, end_node (nur left =
// Wurzel), size.
struct CodecNode {
    CodecNode *left, *right, *parent;
    bool black;
    uint32_t key;
    int value;
};
static_assert(offsetof(CodecNode, key) == 0x1c && offsetof(CodecNode, value) == 0x20);
struct CodecMap {
    CodecNode *begin;
    CodecNode *root;
    size_t size;
    CodecNode *endNode() { return reinterpret_cast<CodecNode *>(&root); }
};

int collect(const CodecNode *node, uint32_t *keys, int *values, int n, int max)
{
    if (!node || n < 0)
        return n;
    n = collect(node->left, keys, values, n, max);
    if (n < 0 || n >= max)
        return -1;
    keys[n] = node->key;
    values[n] = node->value;
    return collect(node->right, keys, values, n + 1, max);
}

void rotateLeft(CodecNode *x)
{
    CodecNode *y = x->right;
    x->right = y->left;
    if (x->right)
        x->right->parent = x;
    y->parent = x->parent;
    if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void rotateRight(CodecNode *x)
{
    CodecNode *y = x->left;
    x->left = y->right;
    if (x->left)
        x->left->parent = x;
    y->parent = x->parent;
    if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->right = x;
    x->parent = y;
}

// Wie libc++ __tree_balance_after_insert.
void balanceAfterInsert(CodecNode *root, CodecNode *x)
{
    x->black = x == root;
    while (x != root && !x->parent->black) {
        CodecNode *grand = x->parent->parent;
        if (x->parent == grand->left) {
            CodecNode *y = grand->right;
            if (y && !y->black) {
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = x == root;
                y->black = true;
            } else {
                if (x != x->parent->left) {
                    x = x->parent;
                    rotateLeft(x);
                }
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = false;
                rotateRight(x);
                break;
            }
        } else {
            CodecNode *y = grand->left;
            if (y && !y->black) {
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = x == root;
                y->black = true;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    rotateRight(x);
                }
                x = x->parent;
                x->black = true;
                x = x->parent;
                x->black = false;
                rotateLeft(x);
                break;
            }
        }
    }
}

// Trägt key → value ein (Knoten mit malloc; libc++ gibt ihn mit operator delete = free frei).
void insert(CodecMap *map, uint32_t key, int value)
{
    auto *node = static_cast<CodecNode *>(malloc(sizeof(CodecNode)));
    *node = CodecNode{nullptr, nullptr, nullptr, false, key, value};
    CodecNode *parent = map->endNode();
    CodecNode **link = &map->root;
    while (*link) {
        parent = *link;
        link = key < parent->key ? &parent->left : &parent->right;
    }
    node->parent = parent;
    *link = node;
    if (map->begin->left)
        map->begin = map->begin->left;
    balanceAfterInsert(map->root, node);
    ++map->size;
}

bool isResolveExecutable()
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return strcmp(base ? base + 1 : exe, "resolve") == 0;
}

void patchResolve()
{
    CodeImage code;
    AacSites sites;
    std::string error;
    if (!runningImage(&code) || !findAacSites(code, &sites, &error)) {
        logMessage("AAC: Stelle in Resolve nicht gefunden (%s), MP4/MOV-Ton bleibt aus; resolve-linux-vst check",
                   error.c_str());
        return;
    }

    auto *map = reinterpret_cast<CodecMap *>(sites.codecMap);
    uint32_t keys[8];
    int values[8];
    const int n = collect(map->root, keys, values, 0, 8);
    const uint32_t wantKeys[] = {0x2e6d7033, 0x61632d33, 0x616c6163, 0x666c6163}; // .mp3 ac-3 alac flac
    const int wantValues[] = {0x15001, 0x15003, 0x15010, 0x1500c};
    if (n != 4 || map->size != 4 || !map->begin || memcmp(keys, wantKeys, sizeof wantKeys) != 0
        || memcmp(values, wantValues, sizeof wantValues) != 0 || map->begin->key != wantKeys[0]) {
        logMessage("AAC: Codec-Tabelle in Resolve unerwartet (%d Einträge), MP4/MOV-Ton bleibt aus", n);
        return;
    }

    resolveVstSwitchTargets[0] = sites.switchAlac;
    resolveVstSwitchTargets[1] = sites.switchFfmpeg;
    resolveVstSwitchTargets[2] = sites.switchDiscard;
    resolveVstCodecTargets[0] = sites.codecOther;
    resolveVstCodecTargets[1] = sites.codecExtradata;
    insert(map, kFourccAac, kCodecAac);
    insert(map, kFourccMp4a, kCodecAac);
    bool ok = writeJump(sites.codecAt, 24, reinterpret_cast<void *>(&resolveVstCodec))
              && writeJump(sites.switchAt, 14, reinterpret_cast<void *>(&resolveVstSwitch));
    using UL = unsigned long;
    if (ok)
        logMessage("AAC: Resolve angepasst (aac/mp4a → FFmpeg; Weiche %#lx, Decoder %#lx, Tabelle %#lx)",
                   UL(sites.switchAt), UL(sites.codecAt), UL(sites.codecMap));
    else
        logMessage("AAC: Anpassen fehlgeschlagen (mprotect)");
}

using MainFunction = int (*)(int, char **, char **);
MainFunction gMain = nullptr;

int wrappedMain(int argc, char **argv, char **env)
{
    patchResolve();
    return gMain(argc, argv, env);
}

} // namespace

// Vor avcodec_open2 (aus resolveVstCodec, nur für AAC): Spurbeschreibung → extradata,
// Vorlauf nicht verwerfen.
void resolveVstAacPrepare(void *decoder, void *context)
{
    auto *ctx = static_cast<uint8_t *>(context);
    *reinterpret_cast<int *>(ctx + kContextFlags2) |= kFlag2SkipManual;
    void *&extradata = *reinterpret_cast<void **>(ctx + kContextExtradata);
    int &extradataSize = *reinterpret_cast<int *>(ctx + kContextExtradataSize);
    if (extradata)
        return;

    void *source = *reinterpret_cast<void **>(static_cast<uint8_t *>(decoder) + 8);
    using Extension = bool (*)(void *, ByteVector *);
    Extension extension = (*reinterpret_cast<Extension **>(source))[0x50 / sizeof(void *)];
    ByteVector data{};
    const bool have = extension(source, &data);
    const size_t n = data.end - data.begin;

    const uint8_t *asc = nullptr;
    size_t ascLen = 0;
    int objectType = -1;
    static std::once_flag logged;
    if (have && audioSpecificConfig(data.begin, n, &asc, &ascLen, &objectType)) {
        using Alloc = void *(*)(size_t);
        static auto avMallocz = reinterpret_cast<Alloc>(dlsym(RTLD_DEFAULT, "av_mallocz"));
        if (avMallocz)
            if (void *p = avMallocz(ascLen + kInputPadding)) {
                memcpy(p, asc, ascLen);
                extradata = p;
                extradataSize = int(ascLen);
            }
        std::call_once(logged, [&] {
            logMessage("AAC: AudioSpecificConfig %s (Spurdaten %s, Objekttyp %#x)", hex(asc, ascLen).c_str(),
                       hex(data.begin, n).c_str(), objectType);
        });
    } else {
        logMessage("AAC: keine AudioSpecificConfig in der Spur (%s, %zu Bytes: %s)", have ? "da" : "fehlt", n,
                   hex(data.begin, n).c_str());
    }
    free(data.begin);
}

// Startet main über wrappedMain, damit patchResolve nach Resolves statischen Konstruktoren
// läuft (die Tabelle entsteht erst dort). Nur im Prozess resolve; ohne AAC-Bibliothek nicht.
EXPORT int __libc_start_main(MainFunction main, int argc, char **argv, void (*init)(), void (*fini)(),
                             void (*rtldFini)(), void *stackEnd)
{
    using F = int (*)(MainFunction, int, char **, void (*)(), void (*)(), void (*)(), void *);
    auto real = reinterpret_cast<F>(dlvsym(RTLD_NEXT, "__libc_start_main", "GLIBC_2.2.5"));
    if (!real)
        real = reinterpret_cast<F>(dlsym(RTLD_NEXT, "__libc_start_main"));
    if (isResolveExecutable() && aacLibrary()) {
        gMain = main;
        main = wrappedMain;
    }
    return real(main, argc, argv, init, fini, rtldFini, stackEnd);
}
