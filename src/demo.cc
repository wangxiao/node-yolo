#include <nan.h>
#include <v8.h>
#include "demo.h"
#include <vector>

extern "C" {
    #include "network.h"
    #include "detection_layer.h"
    #include "region_layer.h"
    #include "cost_layer.h"
    #include "utils.h"
    #include "parser.h"
    #include "box.h"
    #include "image.h"
    #include "demo.h"
    #include <sys/time.h>
    #include "option_list.h"
    #include "data.h"
    #include "opencv2/highgui/highgui_c.h"
    #include "opencv2/imgproc/imgproc_c.h"

    image get_image_from_stream(CvCapture *cap);
}


using v8::Local;
using v8::Function;
using v8::Value;

#define FRAMES 3

static char **demo_names;
static image **demo_alphabet;
static int demo_classes;

static float **probs;
static box *boxes;
static network net;
static image in   ;
static image in_s ;
static image det  ;
static image det_s;
static CvCapture * cap;
static float demo_thresh = 0;

static float *predictions[FRAMES];
static int demo_index = 0;
static image images[FRAMES];
static float *avg;
static image modified_frame;
static image original_frame;

struct Result {
   float x;
   float y;
   float w;
   float h;
   float prob;
   char* name;
};

std::vector<Result> current_results;

void *fetch_in_thread(void *ptr) {
    printf("::fetch_in_thread started\n");
    in = get_image_from_stream(cap);
    if(!in.data){
        error("Stream closed.");
    }
    in_s = resize_image(in, net.w, net.h);
    printf("::fetch_in_thread ended\n");
    return 0;
}

void capture_detections(int num, float thresh, box *boxes, float **probs, char **names, image **alphabet, int classes) {
    int i;
    current_results.clear();
    for (i = 0; i < num; ++i) {
        int cl = max_index(probs[i], classes);
        float prob = probs[i][cl];
        if (prob > thresh) {
            box b = boxes[i];
            printf("x %f, y %f, w %f, h %f \n", b.x, b.y, b.w, b.h);
            printf("%s: %.0f%%\n", names[cl], prob*100);
            current_results.push_back({
                .x = b.x,
                .y = b.y,
                .w = b.w,
                .h = b.h,
                .prob = prob,
                .name = names[cl]
            });
        }
    }
}

void *detect_in_thread(void *ptr) {
    printf("->detect_in_thread started %d \n", demo_index);
    float nms = .4;

    layer l = net.layers[net.n-1];
    float *X = det_s.data;
    float *prediction = network_predict(net, X);

    memcpy(predictions[demo_index], prediction, l.outputs*sizeof(float));
    mean_arrays(predictions, FRAMES, l.outputs, avg);
    l.output = avg;

    free_image(det_s);
    if(l.type == DETECTION){
        get_detection_boxes(l, 1, 1, demo_thresh, probs, boxes, 0);
    } else if (l.type == REGION){
        get_region_boxes(l, 1, 1, demo_thresh, probs, boxes, 0, 0);
    } else {
        error("Last layer must produce detections\n");
    }
    if (nms > 0) do_nms(boxes, probs, l.w*l.h*l.n, l.classes, nms);

    images[demo_index] = det;
    det = images[(demo_index + FRAMES/2 + 1)%FRAMES];
    demo_index = (demo_index + 1)%FRAMES;

    original_frame = copy_image(det);
    draw_detections(det, l.w*l.h*l.n, demo_thresh, boxes, probs, demo_names, demo_alphabet, demo_classes);
    capture_detections(l.w*l.h*l.n, demo_thresh, boxes, probs, demo_names, demo_alphabet, demo_classes);
    modified_frame = copy_image(det);

    printf("->detect_in_thread ended %d \n", demo_index);
    return 0;
}

double get_wall_time() {
    struct timeval time;
    if (gettimeofday(&time,NULL)){
        return 0;
    }
    return (double)time.tv_sec + (double)time.tv_usec * .000001;
}

void callback(Local<Function> cb) {
    image modified = modified_frame;
    image original = original_frame;
    image modified_copy = copy_image(modified);
    image original_copy = copy_image(original);
    if(modified.c == 3) rgbgr_image(modified_copy);
    if(original.c == 3) rgbgr_image(original_copy);
    int x,y,k;

    IplImage *disp = cvCreateImage(cvSize(modified.w,modified.h), IPL_DEPTH_8U, modified.c);
    int step = disp->widthStep;

    char* rawData;

    for(y = 0; y < modified.h; ++y){
        for(x = 0; x < modified.w; ++x){
            for(k= 0; k < modified.c; ++k){
                disp->imageData[y*step + x*modified.c + k] = (unsigned char)(get_pixel(modified_copy,x,y,k)*255);
            }
        }
    }

    CvMat* compressed = cvEncodeImage(".jpg", disp, 0);
    rawData = (char *) malloc(compressed->cols);
    memcpy(rawData, compressed->data.ptr, compressed->cols);


    IplImage *dispOriginal = cvCreateImage(cvSize(original.w,original.h), IPL_DEPTH_8U, original.c);
    int stepOriginal = dispOriginal->widthStep;

    char* rawDataOriginal;

    for(y = 0; y < original.h; ++y){
        for(x = 0; x < original.w; ++x){
            for(k= 0; k < original.c; ++k){
                dispOriginal->imageData[y*stepOriginal + x*original.c + k] = (unsigned char)(get_pixel(original_copy,x,y,k)*255);
            }
        }
    }

    CvMat* compressedOriginal = cvEncodeImage(".jpg", dispOriginal, 0);
    rawDataOriginal = (char *) malloc(compressedOriginal->cols);
    memcpy(rawDataOriginal, compressedOriginal->data.ptr, compressedOriginal->cols);

    const int argc = 3;
    v8::Local<v8::Array> results = Nan::New<v8::Array>();
    int i = 0;
    printf("Predictions size %zu \n", current_results.size());
    for(auto const& result: current_results) {
        v8::Local<v8::Object> obj = Nan::New<v8::Object>();
        obj->Set(Nan::New("x").ToLocalChecked(), Nan::New<v8::Number>(result.x));
        obj->Set(Nan::New("y").ToLocalChecked(), Nan::New<v8::Number>(result.y));
        obj->Set(Nan::New("w").ToLocalChecked(), Nan::New<v8::Number>(result.w));
        obj->Set(Nan::New("h").ToLocalChecked(), Nan::New<v8::Number>(result.h));
        obj->Set(Nan::New("prob").ToLocalChecked(), Nan::New<v8::Number>(result.prob));
        obj->Set(Nan::New("name").ToLocalChecked(), Nan::New<v8::String>(result.name).ToLocalChecked());
        results->Set(i, obj);
        i++;
    }

    v8::Local<v8::Value> argv[argc] = {
        Nan::NewBuffer(rawData, compressed->cols).ToLocalChecked(),
        Nan::NewBuffer(rawDataOriginal, compressedOriginal->cols).ToLocalChecked(),
        results,
    };
    Nan::MakeCallback(Nan::GetCurrentContext()->Global(), cb, argc, argv);

    cvReleaseImage(&disp);
    free_image(modified_copy);

    cvReleaseImage(&dispOriginal);
    free_image(original_copy);
}


void start_demo(v8::Local<v8::Object> opts, Local<Function> cb) {
    printf("Test\n");

    v8::Local<v8::Value> cfgFile = Nan::Get(opts, Nan::New("cfgFile").ToLocalChecked()).ToLocalChecked();
    v8::String::Utf8Value cfgFileStr(cfgFile);
    char* cfgfile = *cfgFileStr;

    v8::Local<v8::Value> weightFile = Nan::Get(opts, Nan::New("weightFile").ToLocalChecked()).ToLocalChecked();
    v8::String::Utf8Value weightFileStr(weightFile);
    char* weightfile = *weightFileStr;

    v8::Local<v8::Value> dataFile = Nan::Get(opts, Nan::New("dataFile").ToLocalChecked()).ToLocalChecked();
    v8::String::Utf8Value dataFileStr(dataFile);
    char* datafile = *dataFileStr;

    v8::Local<v8::Value> namesFile = Nan::Get(opts, Nan::New("namesFile").ToLocalChecked()).ToLocalChecked();
    v8::String::Utf8Value namesFileStr(namesFile);
    char* namesfile = *namesFileStr;

    list *options = read_data_cfg(datafile);
    char classesParam[] = "classes";
    char namesParam[] = "names";
    int classes = option_find_int(options, classesParam, 20);
    char *name_list = option_find_str(options, namesParam, namesfile);
    char **names = get_labels(name_list);
    float thresh = .24;
    int cam_index = 0;
    image **alphabet = load_alphabet();
    demo_names = names;
    demo_alphabet = alphabet;
    demo_classes = classes;
    demo_thresh = thresh;
    net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);

    srand(2222222);

    cap = cvCaptureFromCAM(cam_index);

    if(!cap) error("Couldn't connect to webcam.\n");

    layer l = net.layers[net.n-1];
    int j;

    avg = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < FRAMES; ++j) predictions[j] = (float *) calloc(l.outputs, sizeof(float));
    for(j = 0; j < FRAMES; ++j) {
        images[j] = make_image(1,1,3);
    }

    boxes = (box *)calloc(l.w*l.h*l.n, sizeof(box));
    probs = (float **)calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = (float *)calloc(l.classes, sizeof(float *));

    pthread_t fetch_thread;
    pthread_t detect_thread;

    fetch_in_thread(0);
    det = in;
    det_s = in_s;

    fetch_in_thread(0);
    detect_in_thread(0);
    det = in;
    det_s = in_s;

    for(j = 0; j < FRAMES/2; ++j){
        fetch_in_thread(0);
        detect_in_thread(0);
        det = in;
        det_s = in_s;
    }

    while(1) {
        if(pthread_create(&fetch_thread, 0, fetch_in_thread, 0)) error("Thread creation failed");
        if(pthread_create(&detect_thread, 0, detect_in_thread, 0)) error("Thread creation failed");

        pthread_join(fetch_thread, 0);
        pthread_join(detect_thread, 0);

        callback(cb);

        det   = in;
        det_s = in_s;
    }
}