#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/camera_geometry.h"
#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include "visualization/spectrograms/spectrogram_mesh_topology.h"
#include "visualization/ui_text.h"
#include "visualization/surface_settings.h"
#include "visualization/axis_tick_labels.h"
#include "visualization/spectrograms/spectrogram_selection.h"
#include "core/performance.h"
#include "visualization/quick3d/surface_quick_item.h"
#include "visualization/quick3d/surface_overlay_commands.h"
#include "visualization/quick3d/surface_overlay_item.h"
#include "visualization/quick/quick_ui.h"
#include <visualization/rendering/qt_frame_pump.h>
#include <visualization/rendering/latest_preparation.h>
#include <QQuickWidget>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickOpenGLUtils>
#include <QOpenGLFramebufferObject>
#include <QPointer>
#include <QMutex>
#include <QMutexLocker>
#include <QApplication>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QJsonArray>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include "core_app/touchpad_input.h"
#include "visualization/slice_overlay.h"
#include <QHideEvent>
#include <QGuiApplication>
#include <QPainter>
#include <QLinearGradient>
#include <QOpenGLTexture>
#include <QOpenGLTimerQuery>
#include <QFontMetrics>
#include <cmath>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <limits>
#include <utility>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <tuple>
#include <visualization/render_preparation_policy.h>


namespace {
struct CpuMeshInput {
    static constexpr int empty_ranges[4]{0,100,0,100};
    NumericMatrixSnapshot data;
    SpectrogramSceneGeometry scene{0,0,empty_ranges};
    size_t bands=0,history=0;
    quint64 generation=0;
    SpectrogramAxisMetadata axes;
    QString projection_status;
};
bool sameMeshScene(const SpectrogramSceneGeometry& a,const SpectrogramSceneGeometry& b) {
    return std::tie(a.xMin,a.xMax,a.zMin,a.zMax,a.scaleX,a.scaleZ,a.yMin,a.yMax,a.clipY,a.reverseTime,a.reverseBands,a.reverseAmplitude)==
           std::tie(b.xMin,b.xMax,b.zMin,b.zMax,b.scaleX,b.scaleZ,b.yMin,b.yMax,b.clipY,b.reverseTime,b.reverseBands,b.reverseAmplitude);
}
struct CpuMeshResult {
    CpuMeshInput input;
    std::vector<float> vertices;
    std::vector<std::uint32_t> surface_indices,edge_indices;
    std::vector<QVector3D> face_normals;
    QVariantMap properties;
    SpectrogramPointCache point_cache;
    size_t rows=0,columns=0;
    GLenum primitive=GL_TRIANGLES;
    bool valid=false;
};
class CpuMeshBuilder {
    CpuMeshResult& result;
    const CpuMeshInput& input;
    const SpectrogramSceneGeometry& scene;
    size_t b0=0,b1=0,t0=0,t1=0,bs=1,ts=1,rows=0,columns=0;
    float peakValue=-std::numeric_limits<float>::infinity();
    size_t peakFrame=0,peakBand=0;
    float sample(size_t band,size_t age) const {
        return float(input.axes.value_scale.map(spectrogramHistorySample(input.data,band,age,input.history)));
    }
    void property(const char* key,const QVariant& value){result.properties.insert(QString::fromLatin1(key),value);}
    void prepareBounds(){
        b0=size_t(std::ceil(scene.zMin));b1=std::min(input.bands,size_t(std::floor(scene.zMax))+1);
        t0=size_t(std::ceil(scene.xMin));t1=std::min(input.history,size_t(std::floor(scene.xMax))+1);
        bs=std::max(size_t{1},(b1-b0)/256);ts=std::max(size_t{1},(t1-t0)/256);
        rows=input.data.empty() || b1<=b0?0:(b1-b0+bs-1)/bs;
        columns=t1<=t0?0:(t1-t0+ts-1)/ts;
    }
    void describeInput(){
        property("historyCapacity",qulonglong(input.history));property("historyBands",qulonglong(input.bands));
        property("validHistoryColumns",qulonglong(input.data.empty()?0:std::min(input.history,input.data.front().size())));
        property("sceneScaleX",scene.scaleX);property("sceneScaleZ",scene.scaleZ);
    }
    bool prepareTopology(){
        result.vertices.clear();result.vertices.resize(rows*columns*6);
        const bool changed=result.surface_indices.empty() || result.rows!=rows || result.columns!=columns;
        if(changed && !spectrogramMeshTopology(rows,columns,result.surface_indices)){
            property("geometryUploadVerified",false);return false;
        }
        return true;
    }
    QVector3D normal(size_t band,size_t age) const{
        const auto left=age?age-1:age,right=std::min(age+1,input.history-1);
        const auto lower=band?band-1:band,upper=std::min(band+1,input.bands-1);
        const auto dx=result.point_cache.point(scene,right,band,sample(band,right))-result.point_cache.point(scene,left,band,sample(band,left));
        const auto dz=result.point_cache.point(scene,age,upper,sample(upper,age))-result.point_cache.point(scene,age,lower,sample(lower,age));
        const auto value=QVector3D::crossProduct(dx,dz).normalized();
        return scene.reverseTime!=scene.reverseBands?-value:value;
    }
    void prepareVertex(size_t row,size_t column){
        const auto startBand=std::min(b0+row*bs,b1-1),startAge=std::min(t0+column*ts,t1-1);
        const auto peak=spectrogramPeakSample(input.data,startBand,std::min(startBand+bs,b1),startAge,std::min(startAge+ts,t1),input.history,input.axes.value_scale.source_decibels);
        const auto value=float(input.axes.value_scale.map(peak.value));
        const auto point=result.point_cache.point(scene,peak.age,peak.band,value),direction=normal(peak.band,peak.age);
        if(value>peakValue){peakValue=value;peakFrame=peak.age;peakBand=peak.band;}
        const std::array<float,6> vertex{point.x(),point.y(),point.z(),direction.x(),direction.y(),direction.z()};
        std::copy(vertex.begin(),vertex.end(),result.vertices.begin()+(row*columns+column)*6);
    }
    void prepareVertices(){
        result.point_cache.prepare(scene,b0,b1,t0,t1,input.bands,input.history,[this](size_t band,size_t age){return sample(band,age);});
        for(size_t row=0;row<rows;++row)for(size_t column=0;column<columns;++column)prepareVertex(row,column);
        result.primitive=rows>1 && columns>1?GL_TRIANGLES:rows*columns>1?GL_LINES:GL_POINTS;
    }
    void describePeak(){
        property("surfacePeakAge",qulonglong(peakFrame));
        property("surfacePeakFrame",qulonglong(input.history?input.history-1-peakFrame:0));property("surfacePeakBand",qulonglong(peakBand));
        property("firstDisplayedValue",input.history?sample(0,input.history-1):0.f);property("latestDisplayedValue",sample(0,0));
        property("surfacePrimitive",result.primitive==GL_TRIANGLES?"triangles":result.primitive==GL_LINES?"lines":"points");
    }
    void describeGeometry(){
        float maximum=result.vertices.empty()?0:-std::numeric_limits<float>::infinity();
        for(size_t at=1;at<result.vertices.size();at+=6)maximum=std::max(maximum,result.vertices[at]);
        property("surfaceMaximumHeight",maximum);property("uniqueVertexCount",qulonglong(result.vertices.size()/6));
        property("surfaceIndexCount",int(result.surface_indices.size()));property("vertexCount",int(result.surface_indices.size()));
    }
    QVector3D trianglePoint(GLuint index) const{
        const auto point=result.surface_indices[index];
        return {result.vertices[point*6],result.vertices[point*6+1],result.vertices[point*6+2]};
    }
    QVector3D triangleNormal(GLuint triangle) const{
        return QVector3D::crossProduct(trianglePoint(triangle+1)-trianglePoint(triangle),trianglePoint(triangle+2)-trianglePoint(triangle)).normalized();
    }
    void crease(const QVector3D& first,const QVector3D& second,GLuint begin,GLuint end){
        if(QVector3D::dotProduct(first,second)<0.9961947f)result.edge_indices.insert(result.edge_indices.end(),{result.surface_indices[begin],result.surface_indices[end]});
    }
    void prepareCreaseCell(GLuint cell){
        const GLuint vertex=cell*6;const auto first=triangleNormal(vertex),second=triangleNormal(vertex+3);
        result.face_normals[cell*2]=first;result.face_normals[cell*2+1]=second;
        const auto cellColumns=columns?columns-1:0;
        if(cellColumns && cell%cellColumns)crease(result.face_normals[(cell-1)*2+1],first,vertex,vertex+1);
        if(cellColumns && cell>=cellColumns)crease(result.face_normals[(cell-cellColumns)*2+1],first,vertex+2,vertex);
        crease(first,second,vertex+3,vertex+4);
    }
    void prepareCreases(){
        const auto started=core::perf::now();result.edge_indices.clear();result.face_normals.resize(result.surface_indices.size()/3);
        // Retain face order and the original five-degree crease criterion.
        for(GLuint cell=0;result.primitive==GL_TRIANGLES && cell<GLuint(result.surface_indices.size()/6);++cell)prepareCreaseCell(cell);
        core::perf::record(core::perf::Stage::SpectrogramCreases,started,core::perf::now()-started);
    }
public:
    CpuMeshBuilder(CpuMeshResult& output,const CpuMeshInput& source):result(output),input(source),scene(source.scene){}
    void build(){
        result.input=input;result.valid=false;const auto started=core::perf::now();
        prepareBounds();describeInput();if(!prepareTopology())return;
        prepareVertices();describePeak();
        core::perf::record(core::perf::Stage::SpectrogramMesh,started,core::perf::now()-started);
        describeGeometry();prepareCreases();result.rows=rows;result.columns=columns;result.valid=true;
    }
};

void prepareCpuMesh(CpuMeshResult& result,const CpuMeshInput& input) {
    CpuMeshBuilder(result,input).build();
}
// One owned job and one reusable completion slot. No Qt object or GL access in
// this thread; shutdown joins before renderer-owned resources are destroyed.
class CpuMeshWorker {
public:
    explicit CpuMeshWorker(bool pooled=false){
        if(pooled)pool_=std::make_unique<Pool>([](auto& result,const auto& input){
            try{prepareCpuMesh(result,input);}catch(...){result.valid=false;}
        });
        else thread_=std::thread([this]{run();});
    }
    ~CpuMeshWorker(){if(pool_)return;{std::lock_guard lock(mutex_);stop_=true;}condition_.notify_one();thread_.join();}
    bool submit(CpuMeshInput input,std::function<void()> wake){
        if(pool_)return pool_->submit(std::move(input),std::move(wake));
        std::lock_guard lock(mutex_);if(busy_||ready_)return false;
        input_=std::move(input);wake_=std::move(wake);busy_=true;condition_.notify_one();return true;
    }
    bool take(CpuMeshResult& result){if(pool_)return pool_->take(result);std::lock_guard lock(mutex_);if(!ready_)return false;std::swap(result,result_);ready_=false;return true;}
private:
    using Pool=visualization::rendering::LatestPreparation<CpuMeshInput,CpuMeshResult>;
    std::unique_ptr<Pool> pool_;
    bool takeWork(CpuMeshInput& input,std::function<void()>& wake){
        std::unique_lock lock(mutex_);
        condition_.wait(lock,[this]{return stop_||busy_;});
        if(stop_)return false;
        input=input_;wake=wake_;return true;
    }
    bool completeWork(){
        std::lock_guard lock(mutex_);
        input_={};wake_={};busy_=false;ready_=true;
        return !stop_;
    }
    void run(){
        for(;;){
            CpuMeshInput input;std::function<void()> wake;
            if(!takeWork(input,wake))return;
            try{prepareCpuMesh(result_,input);}catch(...){result_.valid=false;}
            if(!completeWork())return;
            if(wake)wake();
        }
    }
    std::mutex mutex_;std::condition_variable condition_;bool stop_=false,busy_=false,ready_=false;
    CpuMeshInput input_;CpuMeshResult result_;std::function<void()> wake_;std::thread thread_;
};
}

// GUI-owned presentation state and a separately instantiated render-thread core.
// The two instances only exchange values during synchronize(); GL resources never
// cross that boundary, and NumericMatrixSnapshot shares immutable storage.
class SpectrogramRenderer final : public QObject, protected QOpenGLFunctions {
  public:
    explicit SpectrogramRenderer(QObject *parent=nullptr) : QObject(parent),
        frame_pump([this](auto token){
            submission_token=token;
            core::perf::duration(core::perf::Stage::SpectrogramSchedule,core::perf::now()-request_time);
            request_time=0;
            setProperty("renderSubmissions",property("renderSubmissions").toULongLong()+1);
            if(canvas)canvas->update();
        }), buffer(QOpenGLBuffer::VertexBuffer) {frame_pump.setVisible(false);}
    ~SpectrogramRenderer() override {frame_pump.setVisible(false);mesh_worker.reset();if(initialized)cleanup();}
    QPointer<SurfaceQuickItem> canvas;
    QSize viewport{640,480};
    QPalette colors;
    QFont labelFont;
    QString language;
    qreal dpr=1;
    bool visible=true, initialized=false;
    GLuint targetFramebuffer=0;
    QSize targetPixelSize;
    int width() const{return viewport.width();}
    int height() const{return viewport.height();}
    QSize size() const{return viewport;}
    QRect rect() const{return QRect(QPoint{},viewport);}
    bool isVisible() const{return visible;}
    QPalette palette() const{return colors;}
    QFont font() const{return labelFont;}
    qreal devicePixelRatioF() const{return dpr;}
    QOpenGLContext* context() const{return QOpenGLContext::currentContext();}
    GLuint defaultFramebufferObject() const{return targetFramebuffer;}
    QVariantMap feedback() const {
        core::perf::Scope trace(core::perf::Stage::QuickSurfaceFeedbackBuild);
        QVariantMap result;
        const auto propertyNames=dynamicPropertyNames();
        for(const auto& key:propertyNames)
            if(key!="verifyGeometryUpload")result.insert(QString::fromUtf8(key),property(key.constData()));
        result["displayFps"]=fps();
        result["submissionToken"]=qulonglong(submission_token);
        result["renderFrameStartNs"]=qulonglong(last_frame);
        return result;
    }
    void frameConsumed(const QVariantMap& values){
        const auto received=core::perf::now();
        bool validStart=false;
        const auto frameStart=values.value("renderFrameStartNs").toULongLong(&validStart);
        // Retain the actual paint start for diagnostics across the render/GUI
        // boundary; invalid or stale feedback falls back to receipt time.
        last_frame=validStart && frameStart && frameStart<=received && frameStart>=last_frame ? frameStart : received;
        for(auto it=values.begin();it!=values.end();++it)setProperty(it.key().toUtf8().constData(),it.value());
        frame_pump.complete(values.value("submissionToken").toULongLong());
    }
    bool incompatibleInput(const SpectrogramRenderer& source) const{
        return history_capacity!=source.history_capacity || configured_bands!=source.configured_bands ||
            bandSize()!=source.bandSize() || historySize()!=source.historySize() || seen_clear_generation!=source.clear_generation ||
            axes.time_coordinates!=source.axes.time_coordinates || axes.column_coordinates!=source.axes.column_coordinates ||
            axes.age_axis!=source.axes.age_axis || axes.columns!=source.axes.columns || axes.time_title_key!=source.axes.time_title_key ||
            axes.column_title_key!=source.axes.column_title_key || axes.value_title_key!=source.axes.value_title_key || axes.value_scale!=source.axes.value_scale;
    }
    void copySurfaceInput(const SpectrogramRenderer& source){
        submission_token=source.submission_token;position=source.position;orientation=source.orientation;
        // NumericMatrixSnapshot shares immutable sample storage across threads.
        data=source.data;history_capacity=source.history_capacity;configured_bands=source.configured_bands;
        axes=source.axes;projection_status=source.projection_status;options=source.options;
        selected=source.selected;selection=source.selection;gradient=source.gradient;model3d=source.model3d;
        sliceOverlay=source.sliceOverlay;automatic_range=source.automatic_range;
        std::copy(std::begin(source.ranges),std::end(source.ranges),ranges);
    }
    void synchronizeVerification(const SpectrogramRenderer& source){
        const bool verify=source.property("verifyGeometryUpload").toBool();
        if(verify && !property("verifyGeometryUpload").toBool()){upload=true;setProperty("geometryUploadVerified",QVariant{});}
        setProperty("verifyGeometryUpload",verify);
    }
    void copySurfaceAppearance(const SpectrogramRenderer& source){
        viewport=source.viewport;dpr=source.dpr;colors=source.colors;labelFont=source.labelFont;language=source.language;
        render_visible=source.render_visible && source.visible;
    }

    void synchronizeFrom(SpectrogramRenderer& source){
        core::perf::Scope trace(core::perf::Stage::QuickSurfaceSync);const auto priorScene=geometry();
        const bool newSamples=data.storage_identity()!=source.data.storage_identity();
        if(newSamples)setProperty("snapshotRevision",property("snapshotRevision").toULongLong()+1);
        const bool incompatible=incompatibleInput(source);copySurfaceInput(source);
        const bool changed=incompatible || !sameMeshScene(priorScene,geometry());if(changed)++mesh_generation;
        seen_clear_generation=source.clear_generation;upload|=source.upload || newSamples || changed;source.upload=false;mesh_wake_target=&source;
        synchronizeVerification(source);copySurfaceAppearance(source);
    }
    void renderFrame(GLuint target, const QSize& pixels) {
        targetFramebuffer=target;
        targetPixelSize=pixels;
        setProperty("rendererViewportWidth",width());setProperty("rendererViewportHeight",height());
        setProperty("rendererFramebufferWidth",pixels.width());setProperty("rendererFramebufferHeight",pixels.height());
        if(!initialized){initializeGL();initialized=true;}
        // Qt Quick shares GL state with this renderer. The FBO size already
        // includes device pixels; logical item size/DPR must not size this pass.
        glBindFramebuffer(GL_FRAMEBUFFER,targetFramebuffer);
        glViewport(0,0,targetPixelSize.width(),targetPixelSize.height());
        paintGL();
    }
    QVector3D position{0, 1.6f, 2.8f};
    QQuaternion orientation = QQuaternion::fromEulerAngles(-30, 0, 0);
    std::function<void()> prepare;
    std::function<void(const QString &)> shadow_error;
    NumericMatrixSnapshot data;
    bool upload = true;
    bool render_visible=true;
    int gradient = 1, selection = 1;
    bool model3d = true;
    size_t history_capacity=0, configured_bands=0;
    SpectrogramAxisMetadata axes;
    QString projection_status;
    QJsonObject options=SurfaceSettings::defaults();
    QPoint selected{-1, -1};
    bool point_drag=false;
    SliceOverlay sliceOverlay;
    bool beginSliceDrag(QPointF point) {
        return selection>=2 && property("sliceSampleCount").toInt()>0 && sliceOverlay.begin(size(),point);
    }
    void moveSlice(QPointF point) {
        sliceOverlay.move(size(),point);requestUpdate();
    }
    int ranges[4]{0, 100, 0, 100};
    std::pair<float,float> automatic_range{0,1};
    SpectrogramSceneGeometry geometry() const {
        return spectrogramSceneGeometry(historySize(),bandSize(),ranges,options,automatic_range);
    }
    size_t historySize() const { return history_capacity ? history_capacity : data.empty() ? 0 : data.front().size(); }
    size_t bandSize() const { return configured_bands ? configured_bands : data.size(); }
    float sample(size_t band,size_t age) const {return float(axes.value_scale.map(spectrogramHistorySample(data,band,age,historySize())));}
    QGradientStops gradientStops() const {return SurfaceSettings::gradientStops(options);}
    void requestUpdate() {
        if(!request_time)request_time=core::perf::now();
        frame_pump.setVisible(render_visible && visible && !canvas.isNull());
        frame_pump.request();
    }
    void setRenderFps(int fps) {frame_pump.setRate(fps);}
    void setRenderVisible(bool value) {
        if(render_visible==value)return;
        render_visible=value;
        frame_pump.setVisible(value && visible && !canvas.isNull());
        if(!value){endPointDrag();sliceOverlay.end();}
        rate=0;frames=0;last_frame=0;
        if(clock.isValid())start=last=clock.elapsed();
        if(value)requestUpdate();
    }
    void setData(const NumericMatrixSnapshot &value,bool changed=true) {
        core::perf::Scope handoff(core::perf::Stage::SpectrogramHandoff);
        if(value.empty())++clear_generation;
        data = value;
        if(data.empty()){endPointDrag();selected={-1,-1};setProperty("selectedValue",QVariant{});setProperty("selectionMarkerVisible",false);setProperty("sliceSampleCount",0);}
        if(changed){upload = true;requestUpdate();}
    }
    QMatrix4x4 selectionTransform() const {
        QMatrix4x4 projection;
        projection.perspective(45, float(width()) / std::max(1, height()), .005f, 100);
        QMatrix4x4 view;
        view.rotate(orientation.conjugated());
        view.translate(-position);
        return projection*view;
    }
    SpectrogramProjectedSample projectedSample(int frame,int band,const SpectrogramSceneGeometry& scene,const QMatrix4x4& transform) const {
        if(frame<0 || band<0 || size_t(band)>=data.size() || size_t(frame)>=std::min(historySize(),data[size_t(band)].size()))return {};
        if(frame<scene.xMin || frame>scene.xMax || band<scene.zMin || band>scene.zMax)return {};
        auto p = scene.point(frame, band, sample(band,frame));
        if (!model3d) p.setY(-1);
        const auto v = transform * QVector4D(p, 1);
        if (v.w() <= 0 || v.z() < -v.w() || v.z()>v.w())return {};
        const QPointF projected((v.x() / v.w() + 1) * width() / 2, (1 - v.y() / v.w()) * height() / 2);
        return {projected,v.z()/v.w(),QRectF(rect()).contains(projected)};
    }
    SpectrogramProjectedSample projectedSample(int frame,int band) const {
        return projectedSample(frame,band,geometry(),selectionTransform());
    }
    bool pick(QPointF point,bool dragging=false) {
        if(!selection || data.empty() || !bandSize() || !historySize())return false;
        const auto scene=geometry();
        const auto transform=selectionTransform();
        const auto hit=spectrogramNearestPoint(point,{std::max(0,int(std::ceil(scene.xMin))),
            std::min(int(historySize())-1,int(std::floor(scene.xMax))),std::max(0,int(std::ceil(scene.zMin))),
            std::min(int(data.size())-1,int(std::floor(scene.zMax)))},
            [this,&scene,&transform](int frame,int band){return projectedSample(frame,band,scene,transform);},dragging?std::numeric_limits<double>::infinity():30.);
        if(hit.x()<0)return false;
        selected=hit;
        requestUpdate();
        return true;
    }
    bool beginPointDrag(QPointF point) {
        if(!selection)return false;
        const auto marker=projectedSample(selected.x(),selected.y());
        const auto delta=marker.point-point;
        const bool hitMarker=marker.visible && delta.x()*delta.x()+delta.y()*delta.y()<=12*12;
        point_drag=hitMarker || pick(point);
        setProperty("selectionDragging",point_drag);return point_drag;
    }
    void endPointDrag() {
        point_drag=false;setProperty("selectionDragging",false);
    }
    qreal fps() const {
        return render_visible && clock.isValid() && clock.elapsed() - last < 1500 ? rate : 0;
    }

  private:
    visualization::rendering::QtFramePump frame_pump;
    quint64 request_time=0,last_frame=0,submission_token=0;
    GLfloat line_range[2]{1,1};
    std::unique_ptr<QOpenGLTimerQuery> gpu_query;
    bool gpu_pending=false, gpu_attempted=false;
    QOpenGLBuffer buffer;
    QOpenGLBuffer edge_buffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer surface_buffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer floor_buffer{QOpenGLBuffer::VertexBuffer};
    QString grid_signature;
    QString axis_policy_signature;
    std::unique_ptr<QOpenGLShaderProgram> program, label_program, shadow_program;
    GLuint shadow_framebuffer=0, shadow_texture=0, shadow_color=0;
    bool shadow_attempted=false, shadow_ready=false;
    std::unique_ptr<QOpenGLTexture> label_texture;
    QOpenGLBuffer label_buffer{QOpenGLBuffer::VertexBuffer};
    QString label_signature;
    bool axis_text_valid=false;
    SpectrogramAxisMetadata axis_text_metadata;
    std::array<float,9> axis_text_scene{};
    QSize axis_text_viewport;
    QFont axis_text_font;
    QString axis_text_language;
    int axis_text_precision=0;
    size_t axis_text_known_ages=0;
    std::array<int,3> cached_sections{};
    std::array<std::vector<double>,3> cached_tick_values;
    std::array<QStringList,3> cached_tick_labels;
    std::vector<double> cached_time_indices;
    QStringList cached_label_texts;
    std::array<int,3> cached_label_offsets{};
    int cached_title_offset=0;
    QVector<QSizeF> cached_label_sizes;
    QSize atlas_cell{256,32};
    qreal label_dpr=0;
    int label_ascent=0;
    std::array<QStringList,3> diagnostic_tick_labels;
    std::vector<float> vertices;
    int count = 0, capacity = 0, edge_capacity = 0, surface_capacity = 0;
    std::vector<std::uint32_t> surface_indices;
    size_t topology_rows = 0, topology_columns = 0;
    bool topology_uploaded = false;
    GLenum surface_primitive=GL_TRIANGLES;
    std::vector<GLuint> edge_indices;
    std::vector<QVector3D> face_normals;
    std::unique_ptr<CpuMeshWorker> mesh_worker;
    quint64 mesh_submitted_revision=std::numeric_limits<quint64>::max();
    quint64 mesh_submitted_generation=std::numeric_limits<quint64>::max();
    CpuMeshResult mesh_result;
    QPointer<SpectrogramRenderer> mesh_wake_target;
    quint64 mesh_generation=0,clear_generation=0,seen_clear_generation=0;
    QElapsedTimer clock;
    qint64 start = 0, last = 0;
    int frames = 0;
    qreal rate = 0;
    void destroyShadowResources(){
        if(shadow_framebuffer)glDeleteFramebuffers(1,&shadow_framebuffer);
        if(shadow_texture)glDeleteTextures(1,&shadow_texture);
        if(shadow_color)glDeleteRenderbuffers(1,&shadow_color);
        shadow_framebuffer=shadow_texture=shadow_color=0;shadow_attempted=shadow_ready=false;shadow_program.reset();
    }
    void destroyMeshResources(){
        buffer.destroy();edge_buffer.destroy();surface_buffer.destroy();floor_buffer.destroy();program.reset();
        grid_signature.clear();axis_policy_signature.clear();capacity=edge_capacity=surface_capacity=0;topology_uploaded=false;upload=true;
    }
    void destroyLabelResources(){
        label_texture.reset();label_program.reset();label_buffer.destroy();label_signature.clear();
    }

    void cleanup(){
        if(!context())return;
        // Destroy the timer query exactly once while its GL context is current.
        if(gpu_query){gpu_query.reset();gpu_pending=false;}gpu_attempted=false;
        destroyShadowResources();destroyMeshResources();destroyLabelResources();
    }
    bool failShadow(const QString& reason){
        setProperty("shadowReady",false);setProperty("shadowError",reason);options["shadows"]=false;
        if(shadow_error)shadow_error(reason);return false;
    }
    void allocateShadowTexture(){
        glActiveTexture(GL_TEXTURE0);glGenTextures(1,&shadow_texture);glBindTexture(GL_TEXTURE_2D,shadow_texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT16,1024,1024,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    bool allocateShadowFramebuffer(){
        glGenFramebuffers(1,&shadow_framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,shadow_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,shadow_texture,0);
        // Compatibility profiles require a complete color attachment.
        glGenRenderbuffers(1,&shadow_color);glBindRenderbuffer(GL_RENDERBUFFER,shadow_color);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA4,1024,1024);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,shadow_color);
        const auto status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());glBindTexture(GL_TEXTURE_2D,0);return status==GL_FRAMEBUFFER_COMPLETE;
    }

    bool ensureShadowMap(){
        if(shadow_attempted)return shadow_ready;shadow_attempted=true;
        if(context()->isOpenGLES())return failShadow("Shadow mapping is unavailable on this graphics backend.");
        shadow_program=std::make_unique<QOpenGLShaderProgram>();
        shadow_program->addShaderFromSourceCode(QOpenGLShader::Vertex,"attribute vec3 vertex; uniform mat4 lightMvp; void main(){gl_Position=lightMvp*vec4(vertex,1.0);}");
        shadow_program->addShaderFromSourceCode(QOpenGLShader::Fragment,"void main(){gl_FragColor=vec4(1.0);}");
        if(!shadow_program->link())return failShadow("Cannot initialize the shadow-map shader.");
        allocateShadowTexture();if(!allocateShadowFramebuffer())return failShadow("Cannot allocate the shadow depth map on this graphics backend.");
        shadow_ready=true;setProperty("shadowReady",true);return true;
    }
    int bindShadowPass(const QMatrix4x4& matrix){
        glBindFramebuffer(GL_FRAMEBUFFER,shadow_framebuffer);glViewport(0,0,1024,1024);
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_TRUE);glClear(GL_DEPTH_BUFFER_BIT);
        shadow_program->bind();shadow_program->setUniformValue("lightMvp",matrix);buffer.bind();
        const auto point=shadow_program->attributeLocation("vertex");shadow_program->enableAttributeArray(point);
        shadow_program->setAttributeBuffer(point,GL_FLOAT,0,3,6*sizeof(float));return point;
    }
    void releaseShadowPass(int point){
        shadow_program->disableAttributeArray(point);buffer.release();shadow_program->release();
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());
        glViewport(0,0,targetPixelSize.width(),targetPixelSize.height());
    }

    void renderShadowMap(const QMatrix4x4& lightMvp){
        core::perf::Scope trace(core::perf::Stage::SpectrogramShadow);const auto point=bindShadowPass(lightMvp);
        surface_buffer.bind();glDrawElements(surface_primitive,count,GL_UNSIGNED_INT,nullptr);surface_buffer.release();releaseShadowPass(point);
        setProperty("shadowPasses",property("shadowPasses").toULongLong()+1);
    }
    void initializeSurfaceShader(){
        program = std::make_unique<QOpenGLShaderProgram>();
        program->addShaderFromSourceCode(
            QOpenGLShader::Vertex, "attribute vec3 vertex; attribute vec3 normal; uniform mat4 mvp; uniform bool "
            "model3d; uniform bool reverseAmplitude; uniform bool floorPass; uniform vec2 floorSigns; uniform vec2 gridExtent; varying float height; varying vec2 cell; varying vec3 surfaceNormal; varying vec3 worldPosition; "
            "void main(){vec3 p=vertex; if(floorPass){if(abs(p.x)>gridExtent.x)p.x*=floorSigns.x;if(abs(p.z)>gridExtent.y)p.z*=floorSigns.y;} worldPosition=p; height=reverseAmplitude ? (1.0-vertex.y)/2.0 : (vertex.y+1.0)/2.0;cell=vertex.xz;surfaceNormal=model3d ? normal : "
                                   "vec3(0.0,1.0,0.0);gl_Position=mvp*vec4(p.x,model3d ? p.y : "
                                   "-1.0,p.z,1.0);}");
        program->addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            "varying float height; varying vec2 cell; varying vec3 surfaceNormal; varying vec3 worldPosition; "
            "uniform vec3 lightPosition; uniform vec3 eyePosition; uniform vec3 lightColor; "
            "uniform bool shadowsEnabled; uniform sampler2D shadowMap; uniform mat4 shadowMvp; uniform bool lightingEnabled; uniform float brightness; uniform float lightStrength; uniform float ambientStrength; uniform int gradient; uniform int "
            "selection; "
            "uniform vec2 selected; uniform bool gridPass; uniform vec3 gridColor; uniform float gridOpacity; "
            "uniform int stopCount; uniform float stopPositions[16]; uniform vec3 stopColors[16]; void "
            "main(){if(gridPass){gl_FragColor=vec4(gridColor,gridOpacity);return;} float h=clamp(height,0.0,1.0); vec3 c=stopColors[0]; "
            "for(int i=1;i<16;++i){if(i<stopCount){float t=clamp((h-stopPositions[i-1])/max(0.00001,stopPositions[i]-stopPositions[i-1]),0.0,1.0); c=mix(c,stopColors[i],t);}} "
            "vec3 l=normalize(lightPosition-worldPosition); vec3 n=normalize(surfaceNormal); float d=max(length(lightPosition-worldPosition),0.001); float diffuse=clamp(dot(n,l),0.0,1.0); float specular=clamp(dot(normalize(eyePosition-worldPosition),reflect(-l,n)),0.0,1.0); float visibility=1.0; if(shadowsEnabled){vec4 sp=shadowMvp*vec4(worldPosition,1.0); vec3 uv=sp.xyz/sp.w*0.5+0.5; if(sp.w>0.0 && uv.x>0.0 && uv.x<1.0 && uv.y>0.0 && uv.y<1.0 && uv.z<1.0){float bias=0.001+0.002*(1.0-diffuse); visibility=0.0; for(int x=0;x<2;++x)for(int y=0;y<2;++y){float depth=texture2D(shadowMap,uv.xy+(vec2(float(x),float(y))-0.5)/1024.0).r; visibility+=uv.z-bias<=depth ? 0.25 : 0.0;}}} if(lightingEnabled)c=lightColor*(ambientStrength*c+visibility*lightStrength*(c*pow(diffuse,2.0)+vec3(pow(specular,10.0)))/d); else c*=0.45+0.55*visibility; c*=brightness; "
            "bool hit=selection==1 ? distance(cell,selected)<0.02 : selection==2 ? abs(cell.x-selected.x)<0.01 : "
            "selection==3 ? abs(cell.y-selected.y)<0.01 : false; if(hit)c=mix(c,vec3(1.0,0.85,0.25),0.7);"
            "gl_FragColor=vec4(c,1.0);}");
        const bool linked = program->link();
        setProperty("rendererReady", linked);

    }
    void initializeLabelShader(){
        label_program = std::make_unique<QOpenGLShaderProgram>();
        label_program->addShaderFromSourceCode(
            QOpenGLShader::Vertex,
            "attribute vec2 point; uniform mat4 mvp; uniform vec3 anchor; uniform vec2 offset; "
            "uniform vec2 viewport; uniform vec2 cellSize; uniform float angle; uniform vec4 region; varying vec2 uv; void main(){vec4 p=mvp*vec4(anchor,1.0); "
            "vec2 text=point*cellSize; text=mat2(cos(angle),-sin(angle),sin(angle),cos(angle))*text; p.xy+=vec2(2.0/viewport.x,-2.0/viewport.y)*(offset+text)*p.w; "
            "p.z-=0.000001*p.w;gl_Position=p;uv=region.xy+vec2(point.x,1.0-point.y)*region.zw;}");
        label_program->addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            "uniform sampler2D atlas; varying vec2 uv; void main(){vec4 c=texture2D(atlas,uv); "
            "if(c.a<0.01)discard;gl_FragColor=c;}");
        setProperty("axisLabelsReady", label_program->link());

    }
    void initializeBuffers(){
        const float label_quad[]{0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
        label_buffer.create();
        label_buffer.bind();
        label_buffer.allocate(label_quad, sizeof(label_quad));
        label_buffer.release();
        buffer.create();
        edge_buffer.create();
        surface_buffer.create();
        floor_buffer.create();

    }

    void initializeGL(){
        initializeOpenGLFunctions();glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE,line_range);
        initializeSurfaceShader();initializeLabelShader();initializeBuffers();
        upload=true;start=last=0;frames=0;clock.start();
    }
        struct RestoreNewest {
            SpectrogramRenderer& owner;
            NumericMatrixSnapshot data;
            SpectrogramAxisMetadata axes;
            QString status;
            bool active=false;
            explicit RestoreNewest(SpectrogramRenderer& value):owner(value){}
            ~RestoreNewest(){if(active){owner.data=std::move(data);owner.axes=std::move(axes);owner.projection_status=std::move(status);}}
            void retain(){data=owner.data;axes=owner.axes;status=owner.projection_status;active=true;}
        };
    struct SurfaceFrame {
        QColor background,ink;
        SpectrogramSceneGeometry scene;
        QMatrix4x4 projection,view,shadowMvp;
        QVector3D lightPosition;
        bool shadows=false;
        QLocale axisLocale;
        int precision=0,location=0,normal_location=0;
        size_t filled=0,knownAges=0;
        float wallX=1,wallZ=1;
        QGradientStops stops;
        SurfaceFrame(SpectrogramRenderer& owner,const QColor& backgroundColor,const QColor& textColor):
            background(backgroundColor),ink(textColor),scene(owner.geometry()),
            axisLocale(owner.language=="ru"?"ru_RU":"en_US"),precision(owner.options["axis_precision"].toInt(0)){
            filled=owner.data.empty()?0:std::min(owner.historySize(),owner.data.front().size());
            knownAges=owner.axes.time_coordinates?std::min(filled,owner.axes.ages.size()):filled;
            wallX=owner.position.x()>=0?1.f:-1.f;wallZ=owner.position.z()>=0?1.f:-1.f;
        }
    };

    void submitMesh(bool pooled,bool ready,const SpectrogramSceneGeometry& scene){
        const auto revision=property("snapshotRevision").toULongLong();
        if(!((!pooled&&!ready)||(pooled&&(revision!=mesh_submitted_revision||mesh_generation!=mesh_submitted_generation))))return;
        const auto target=mesh_wake_target;QObject* dispatcher=QCoreApplication::instance();
        if(mesh_worker->submit({data,scene,bandSize(),historySize(),mesh_generation,axes,projection_status},[dispatcher,target]{
            QMetaObject::invokeMethod(dispatcher,[target]{if(target)target->requestUpdate();},Qt::QueuedConnection);
        })){
            mesh_submitted_revision=revision;mesh_submitted_generation=mesh_generation;
            setProperty("meshWorkerJobs",property("meshWorkerJobs").toULongLong()+1);
        }
    }
    bool takeMesh(){
        if(!mesh_worker->take(mesh_result))return false;
        if(mesh_result.valid && mesh_result.input.generation==mesh_generation)return true;
        setProperty("meshWorkerStale",property("meshWorkerStale").toULongLong()+1);return false;
    }
    bool prepareMeshResult(){
        const auto scene=geometry();
        if(presentation::RenderPreparationPolicy::mesh==presentation::MeshPreparation::Inline){
            prepareCpuMesh(mesh_result,{data,scene,bandSize(),historySize(),mesh_generation,axes,projection_status});
            return mesh_result.valid;
        }
        const bool pooled=presentation::RenderPreparationPolicy::mesh==presentation::MeshPreparation::Pooled;
        if(!mesh_worker)mesh_worker=std::make_unique<CpuMeshWorker>(pooled);
        const bool ready=takeMesh();submitMesh(pooled,ready,scene);return ready && mesh_result.valid;
    }
    bool acceptMeshTopology(){
        const auto recycledRows=topology_rows,recycledColumns=topology_columns;
        vertices.swap(mesh_result.vertices);surface_indices.swap(mesh_result.surface_indices);edge_indices.swap(mesh_result.edge_indices);
        const auto rows=mesh_result.rows,columns=mesh_result.columns;
        const bool changed=!topology_uploaded || topology_rows!=rows || topology_columns!=columns;
        topology_rows=rows;topology_columns=columns;surface_primitive=mesh_result.primitive;count=int(surface_indices.size());
        mesh_result.rows=recycledRows;mesh_result.columns=recycledColumns;
        return changed;
    }
    void describeAcceptedMesh(){
        setProperty("meshWorkerAccepted",property("meshWorkerAccepted").toULongLong()+(mesh_worker?1:0));
        for(auto it=mesh_result.properties.cbegin();it!=mesh_result.properties.cend();++it)setProperty(it.key().toUtf8().constData(),it.value());
        setProperty("geometryUploads",property("geometryUploads").toULongLong()+1);
    }
    bool uploadVertices(){
        buffer.bind();const int bytes=int(vertices.size()*sizeof(float));setProperty("geometryVertexBytes",bytes);
        if(bytes>capacity){capacity=bytes;buffer.allocate(nullptr,capacity);}
        if(bytes)buffer.write(0,vertices.data(),bytes);
        bool verified=true;
        if(property("verifyGeometryUpload").toBool()){
            std::vector<float> readback(vertices.size());
            verified=!bytes || (buffer.read(0,readback.data(),bytes) && std::memcmp(vertices.data(),readback.data(),std::size_t(bytes))==0);
        }
        buffer.release();return verified;
    }
    void verifySurfaceIndices(int bytes,bool verified){
        if(!property("verifyGeometryUpload").toBool())return;
        std::vector<std::uint32_t> readback(surface_indices.size());
        verified=verified && (!bytes || (surface_buffer.read(0,readback.data(),bytes) && std::memcmp(surface_indices.data(),readback.data(),std::size_t(bytes))==0));
        setProperty("geometryUploadVerified",verified);
    }
    void uploadSurfaceIndices(bool changed,bool verified){
        const int bytes=int(surface_indices.size()*sizeof(std::uint32_t));surface_buffer.bind();
        if(changed){
            if(bytes>surface_capacity){surface_capacity=bytes;surface_buffer.allocate(nullptr,surface_capacity);}
            if(bytes)surface_buffer.write(0,surface_indices.data(),bytes);
            topology_uploaded=true;setProperty("topologyUploads",property("topologyUploads").toULongLong()+1);
        }
        verifySurfaceIndices(bytes,verified);
        surface_buffer.release();setProperty("geometryIndexBytes",bytes);
        setProperty("geometryUploadedBytes",property("geometryVertexBytes").toInt()+(changed?bytes:0));
    }
    void uploadEdges(){
        const auto started=core::perf::now();edge_buffer.bind();
        const int bytes=int(edge_indices.size()*sizeof(GLuint));
        if(bytes>edge_capacity){edge_capacity=bytes;edge_buffer.allocate(nullptr,edge_capacity);}
        if(bytes)edge_buffer.write(0,edge_indices.data(),bytes);
        edge_buffer.release();core::perf::record(core::perf::Stage::SpectrogramUpload,started,core::perf::now()-started);
        setProperty("geometryUploadedBytes",property("geometryUploadedBytes").toLongLong()+bytes);
        setProperty("wireframeEdgeCount",int(edge_indices.size()/2));
    }
    void uploadMesh(bool changed){
        const auto started=core::perf::now();const bool verified=uploadVertices();uploadSurfaceIndices(changed,verified);
        core::perf::record(core::perf::Stage::SpectrogramUpload,started,core::perf::now()-started);uploadEdges();
    }
    void wakeNewestMesh(){
        // The last ingress frame must not be stranded behind an older packet.
        const auto target=mesh_wake_target;
        QMetaObject::invokeMethod(QCoreApplication::instance(),[target]{if(target)target->requestUpdate();},Qt::QueuedConnection);
    }

    bool prepareMesh(RestoreNewest& newest){
        if(!upload)return true;
        core::perf::Scope trace(core::perf::Stage::SpectrogramGeometry);
        if(!prepareMeshResult())return false;
        const bool older=mesh_result.input.data.storage_identity()!=data.storage_identity();
        if(mesh_worker)adoptPreparedInput(newest);
        const bool changed=acceptMeshTopology();describeAcceptedMesh();uploadMesh(changed);
        upload=older;if(older)wakeNewestMesh();return true;
    }
    void adoptPreparedInput(RestoreNewest& newest){
        newest.retain();data=mesh_result.input.data;axes=mesh_result.input.axes;
        projection_status=mesh_result.input.projection_status;
    }
    void prepareShadows(SurfaceFrame& frame){
        frame.lightPosition=orientation.rotatedVector({0,0,9.5f})+QVector3D(0,.5f,0);
        QMatrix4x4 projection,view;
        // Near=3 preserves depth precision for the scene-attached light.
        projection.perspective(60,1,3.f,30);view.lookAt(frame.lightPosition,{0,0,0},orientation.rotatedVector({0,1,0}));
        frame.shadowMvp=projection*view;frame.shadows=options["shadows"].toBool() && ensureShadowMap();
        if(frame.shadows)renderShadowMap(frame.shadowMvp);
        setProperty("shadowsActive",frame.shadows);
    }
    void bindSurfaceProgram(SurfaceFrame& frame){
        frame.projection.perspective(45,float(width())/std::max(1,height()),.005f,100.f);
        frame.view.rotate(orientation.conjugated());frame.view.translate(-position);program->bind();
        program->setUniformValue("mvp",frame.projection*frame.view);program->setUniformValue("eyePosition",position);
        program->setUniformValue("shadowsEnabled",frame.shadows);program->setUniformValue("shadowMvp",frame.shadowMvp);program->setUniformValue("shadowMap",1);
        if(frame.shadows){glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,shadow_texture);glActiveTexture(GL_TEXTURE0);}
        program->setUniformValue("lightPosition",frame.lightPosition);
    }
    void bindSurfaceMaterial(){
        const auto lightColor=QColor(Qt::white);
        program->setUniformValue("lightColor",QVector3D(lightColor.redF(),lightColor.greenF(),lightColor.blueF()));
        program->setUniformValue("lightStrength",5.f*float(options["light_strength"].toDouble(1)));
        program->setUniformValue("ambientStrength",.5f*float(options["ambient_strength"].toDouble(1)));
        program->setUniformValue("lightingEnabled",options["light"].toBool(true));program->setUniformValue("brightness",float(options["brightness"].toDouble(1)));
        program->setUniformValue("gradient",gradient);
    }
    void bindGradient(SurfaceFrame& frame){
        frame.stops=gradientStops();std::array<GLfloat,16> offsets{};std::array<QVector3D,16> colors{};
        const auto stopCount=std::min(16,int(frame.stops.size()));
        for(int index=0;index<stopCount;++index){
            offsets[index]=frame.stops[index].first;const auto color=frame.stops[index].second;
            colors[index]={float(color.redF()),float(color.greenF()),float(color.blueF())};
        }
        program->setUniformValue("stopCount",stopCount);program->setUniformValueArray("stopPositions",offsets.data(),stopCount,1);
        program->setUniformValueArray("stopColors",colors.data(),stopCount);
    }
    void bindSurfaceSelection(const SurfaceFrame& frame){
        program->setUniformValue("gridOpacity",1.f);program->setUniformValue("model3d",model3d);
        program->setUniformValue("gridPass",false);program->setUniformValue("floorPass",false);program->setUniformValue("selection",selection);
        program->setUniformValue("reverseAmplitude",frame.scene.reverseAmplitude);
        const auto point=frame.scene.point(selected.x(),selected.y(),0);
        program->setUniformValue("selected",selected.x()<0?QVector2D(-100,-100):QVector2D(point.x(),point.z()));
    }
    void bindSurfaceAttributes(SurfaceFrame& frame){
        buffer.bind();frame.location=program->attributeLocation("vertex");program->enableAttributeArray(frame.location);
        program->setAttributeBuffer(frame.location,GL_FLOAT,0,3,6*sizeof(float));
        frame.normal_location=program->attributeLocation("normal");program->enableAttributeArray(frame.normal_location);
        program->setAttributeBuffer(frame.normal_location,GL_FLOAT,3*sizeof(float),3,6*sizeof(float));
    }
    void drawFilledSurface(){
        // Offset only fill, leaving depth-tested edge positions on the exact mesh.
        glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1.f,1.f);surface_buffer.bind();
        glDrawElements(surface_primitive,count,GL_UNSIGNED_INT,nullptr);surface_buffer.release();glDisable(GL_POLYGON_OFFSET_FILL);
    }
    void bindWireframe(){
        program->setUniformValue("gridPass",true);
        const auto setting=options["mesh_color"].toString("auto");const QColor color=setting=="auto"?palette().color(QPalette::Shadow):QColor(setting);
        program->setUniformValue("gridColor",QVector3D(color.redF(),color.greenF(),color.blueF()));
        program->setUniformValue("gridOpacity",float(options["mesh_contrast"].toDouble(1)));
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);
        const auto width=std::clamp(float(options["mesh_width"].toDouble(1)),line_range[0],line_range[1]);
        glLineWidth(width);setProperty("meshActualWidth",width);
    }
    void drawWireframe(){
        bindWireframe();edge_buffer.bind();
        if(options["mesh_grid"].toBool(true))glDrawElements(GL_LINES,int(edge_indices.size()),GL_UNSIGNED_INT,nullptr);
        edge_buffer.release();glDisable(GL_BLEND);glLineWidth(1);program->setUniformValue("gridOpacity",1.f);
        glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);buffer.release();
    }

    void drawSurface(SurfaceFrame& frame){
        prepareShadows(frame);const auto started=core::perf::now();
        bindSurfaceProgram(frame);bindSurfaceMaterial();bindGradient(frame);bindSurfaceSelection(frame);
        bindSurfaceAttributes(frame);drawFilledSurface();drawWireframe();
        core::perf::record(core::perf::Stage::SpectrogramSurface,started,core::perf::now()-started);
    }
    static std::array<float,9> axisScene(const SurfaceFrame& frame){
        const auto& scene=frame.scene;
        return {scene.xMin,scene.xMax,scene.zMin,scene.zMax,scene.yMin,scene.yMax,float(scene.reverseTime),float(scene.reverseBands),float(scene.reverseAmplitude)};
    }
    bool axisTextDirty(const SurfaceFrame& frame) const{
        return !axis_text_valid || axis_text_scene!=axisScene(frame) || axis_text_viewport!=size() || axis_text_font!=font() ||
            axis_text_language!=language || axis_text_precision!=frame.precision || axis_text_known_ages!=frame.knownAges || axis_text_metadata!=axes;
    }
    int readableAxisSections(const SurfaceFrame& frame,int axis,int candidate,bool initial) const{
        const auto& scene=frame.scene;
        if(axis==0 && (initial || !axes.time_coordinates))return spectrogramReadableSections(scene.xMax-scene.xMin,candidate,!axes.time_coordinates);
        if(axis==1 && (initial || !axes.column_coordinates))return spectrogramReadableSections(scene.zMax-scene.zMin,candidate,!axes.column_coordinates);
        if(axis==2)return std::max(height()>=220?4:1,spectrogramReadableSections(double(scene.yMax)-scene.yMin,candidate,false));
        return candidate;
    }
    void buildAxisTickValues(const SurfaceFrame& frame,int axis,int candidate){
        auto& values=cached_tick_values[axis];values.clear();
        if(axis==0)cached_time_indices=spectrogramValidTimeTicks(frame.scene,frame.knownAges,candidate,!axes.time_coordinates);
        for(int tick=0;tick<=candidate;++tick){
            const auto fraction=float(tick)/candidate;
            values.push_back(axis==0?axes.age(cached_time_indices[tick]):axis==1?axes.column(frame.scene.bandTick(fraction)):frame.scene.signalTick(fraction));
        }
        cached_tick_labels[axis]=axisTickLabels(values,frame.axisLocale,frame.precision);
    }
    void buildAxisText(const SurfaceFrame& frame,int axis,const QFontMetrics& metrics){
        int candidate=spectrogramAxisSections(size(),{double(metrics.horizontalAdvance("000")),double(metrics.height())},axis);
        candidate=readableAxisSections(frame,axis,candidate,true);
        for(;;){
            buildAxisTickValues(frame,axis,candidate);
            int widest=0;for(const auto& text:std::as_const(cached_tick_labels[axis]))widest=std::max(widest,metrics.horizontalAdvance(text));
            const auto fitting=spectrogramAxisSections(size(),{double(widest),double(metrics.height())},axis);
            if(candidate<=fitting || candidate==1)break;
            candidate=readableAxisSections(frame,axis,std::min(candidate-1,fitting),false);
        }
        cached_sections[axis]=candidate;
    }
    void publishAxisTickValues(int axis,QString& signature){
        constexpr const char* properties[]{"timeAxisTickValues","bandAxisTickValues","valueAxisTickValues"};
        QVariantList values;
        for(double value:cached_tick_values[axis]){
            values.append(value);signature+=QString::number(value,'g',17)+QLatin1Char('/');
        }
        signature+=QLatin1Char('|');
        setProperty(properties[axis],values);
    }
    void publishAxisPolicy(){
        setProperty("timeAxisSections",cached_sections[0]);setProperty("bandAxisSections",cached_sections[1]);setProperty("valueAxisSections",cached_sections[2]);
        QString signature;
        for(int axis=0;axis<3;++axis)publishAxisTickValues(axis,signature);
        setProperty("timeAxisTitle",axes.time_title_key);setProperty("bandAxisTitle",axes.column_title_key);
        if(axis_policy_signature!=signature){axis_policy_signature=signature;setProperty("axisPolicyRevision",property("axisPolicyRevision").toULongLong()+1);}
    }
    void cacheAxisText(const SurfaceFrame& frame){
        cached_label_texts.clear();
        for(int axis=0;axis<3;++axis){cached_label_offsets[axis]=int(cached_label_texts.size());cached_label_texts.append(cached_tick_labels[axis]);}
        cached_title_offset=int(cached_label_texts.size());
        cached_label_texts<<ui::text(axes.time_title_key.toUtf8().constData())<<ui::text(axes.column_title_key.toUtf8().constData())<<ui::text(axes.value_title_key.toUtf8().constData());
        axis_text_scene=axisScene(frame);axis_text_viewport=size();axis_text_font=font();axis_text_language=language;
        axis_text_precision=frame.precision;axis_text_known_ages=frame.knownAges;axis_text_metadata=axes;axis_text_valid=true;
        setProperty("axisTextLayoutBuilds",property("axisTextLayoutBuilds").toULongLong()+1);
    }
    void prepareAxisText(const SurfaceFrame& frame){
        setProperty("validAgeCount",qulonglong(frame.knownAges));
        if(axisTextDirty(frame)){
            const QFontMetrics metrics(font());for(int axis=0;axis<3;++axis)buildAxisText(frame,axis,metrics);
            publishAxisPolicy();cacheAxisText(frame);
        }
        setProperty("horizontalGridLines",options["axis_grid"].toBool(true)?cached_sections[2]+1:0);
        setProperty("horizontalGridLinesTotal",options["axis_grid"].toBool(true)?2*(cached_sections[2]+1):0);
    }
    float timeGridX(const SurfaceFrame& frame,int tick) const{
        return std::isfinite(cached_time_indices[tick])?frame.scene.point(float(cached_time_indices[tick]),frame.scene.zMin,frame.scene.yMin).x():frame.scene.scaleX*(-1+2.f*tick/cached_sections[0]);
    }
    static void gridLine(std::vector<float>& output,const QVector3D& first,const QVector3D& second){
        output.insert(output.end(),{first.x(),first.y(),first.z(),0,1,0,second.x(),second.y(),second.z(),0,1,0});
    }
    static void gridFace(std::vector<float>& output,QVector3D a,QVector3D b,QVector3D c,QVector3D d){
        for(auto point:{a,b,c,a,c,d})output.insert(output.end(),{point.x(),point.y(),point.z(),0,1,0});
    }
    void floorGridLines(const SurfaceFrame& frame,std::vector<float>& output) const{
        const auto sx=frame.scene.scaleX+.1f,sz=frame.scene.scaleZ+.1f;constexpr float y=-1.01f;
        for(int tick=0;tick<=cached_sections[0];++tick){const auto x=timeGridX(frame,tick);gridLine(output,{x,y,-sz},{x,y,sz});}
        for(int tick=0;tick<=cached_sections[1];++tick){const auto z=frame.scene.scaleZ*(1-2.f*tick/cached_sections[1]);gridLine(output,{-sx,y,z},{sx,y,z});}
    }
    void wallGridLines(const SurfaceFrame& frame,std::vector<float>& output) const{
        const auto sx=frame.scene.scaleX+.1f,sz=frame.scene.scaleZ+.1f;
        for(int tick=0;tick<=cached_sections[0];++tick){const auto x=timeGridX(frame,tick);gridLine(output,{x,-1,-sz},{x,1,-sz});}
        for(int tick=0;tick<=cached_sections[1];++tick){const auto z=frame.scene.scaleZ*(1-2.f*tick/cached_sections[1]);gridLine(output,{-sx,-1,z},{-sx,1,z});}
        for(int tick=0;tick<=cached_sections[2];++tick){const auto y=-1+2.f*tick/cached_sections[2];gridLine(output,{-sx,y,-sz},{sx,y,-sz});gridLine(output,{-sx,y,-sz},{-sx,y,sz});}
    }
    void wallGridFaces(const SurfaceFrame& frame,std::vector<float>& output) const{
        const auto sx=frame.scene.scaleX+.1f,sz=frame.scene.scaleZ+.1f;constexpr float y=-1.01f;
        gridFace(output,{-sx,y,-sz},{sx,y,-sz},{sx,y,sz},{-sx,y,sz});
        gridFace(output,{-sx,-1,-sz},{sx,-1,-sz},{sx,1,-sz},{-sx,1,-sz});
        gridFace(output,{-sx,-1,-sz},{-sx,-1,sz},{-sx,1,sz},{-sx,1,-sz});
    }
    void prepareWallGrid(const SurfaceFrame& frame){
        const auto& scene=frame.scene;
        const auto signature=QString("%1/%2/%3/%4/%5/%6/%7/%8/%9").arg(cached_sections[0]).arg(cached_sections[1]).arg(cached_sections[2])
            .arg(scene.scaleX,0,'g',9).arg(scene.scaleZ,0,'g',9).arg(qulonglong(frame.knownAges)).arg(scene.xMin,0,'g',9).arg(scene.xMax,0,'g',9).arg(scene.reverseTime);
        if(grid_signature==signature)return;
        std::vector<float> output;floorGridLines(frame,output);wallGridLines(frame,output);wallGridFaces(frame,output);
        floor_buffer.bind();floor_buffer.allocate(output.data(),int(output.size()*sizeof(float)));floor_buffer.release();
        grid_signature=signature;setProperty("gridRevision",property("gridRevision").toULongLong()+1);
    }
    void bindWallGrid(const SurfaceFrame& frame){
        floor_buffer.bind();program->setUniformValue("floorPass",true);program->setUniformValue("floorSigns",QVector2D(frame.wallX,frame.wallZ));
        program->setUniformValue("gridExtent",QVector2D(frame.scene.scaleX+.05f,frame.scene.scaleZ+.05f));
        program->setAttributeBuffer(frame.location,GL_FLOAT,0,3,6*sizeof(float));program->setAttributeBuffer(frame.normal_location,GL_FLOAT,3*sizeof(float),3,6*sizeof(float));
        program->setUniformValue("gridPass",true);program->setUniformValue("model3d",true);
        const auto color=palette().color(QPalette::Mid);program->setUniformValue("gridColor",QVector3D(color.redF(),color.greenF(),color.blueF()));
    }
    void drawWallFaces(const SurfaceFrame& frame,int first){
        const auto color=frame.background;program->setUniformValue("gridColor",QVector3D(color.redF(),color.greenF(),color.blueF()));
        glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1,1);glDrawArrays(GL_TRIANGLES,first,18);glDisable(GL_POLYGON_OFFSET_FILL);
        const auto grid=palette().color(QPalette::Mid);program->setUniformValue("gridColor",QVector3D(grid.redF(),grid.greenF(),grid.blueF()));
    }
    void releaseWalls(const SurfaceFrame& frame){
        const auto color=frame.ink;program->setUniformValue("gridColor",QVector3D(color.redF(),color.greenF(),color.blueF()));
        floor_buffer.release();program->disableAttributeArray(frame.location);program->disableAttributeArray(frame.normal_location);buffer.release();program->release();
    }

    void drawWalls(SurfaceFrame& frame){
        const auto started=core::perf::now();prepareAxisText(frame);prepareWallGrid(frame);bindWallGrid(frame);
        const auto floorVertices=2*(cached_sections[0]+cached_sections[1]+2);
        const auto wallVertices=2*(cached_sections[0]+cached_sections[1]+2)+4*(cached_sections[2]+1);
        drawWallFaces(frame,floorVertices+wallVertices);
        if(options["floor_grid"].toBool(true))glDrawArrays(GL_LINES,0,floorVertices);
        if(options["axis_grid"].toBool(true))glDrawArrays(GL_LINES,floorVertices,wallVertices);
        releaseWalls(frame);core::perf::record(core::perf::Stage::SpectrogramWalls,started,core::perf::now()-started);
    }
    void recordLabelDiagnostics(){
        if(!core::perf::enabled.load(std::memory_order_relaxed))return;
        constexpr const char* changes[]{"labelTimeTextChanges","labelBandTextChanges","labelValueTextChanges"};
        constexpr const char* labels[]{"labelTimeTexts","labelBandTexts","labelValueTexts"};
        for(int axis=0;axis<3;++axis)if(diagnostic_tick_labels[axis]!=cached_tick_labels[axis]){
            diagnostic_tick_labels[axis]=cached_tick_labels[axis];
            setProperty(changes[axis],property(changes[axis]).toULongLong()+1);
            setProperty(labels[axis],cached_tick_labels[axis]);
        }
    }
    void describeAtlasBuild(){
        setProperty("axisGlyphBuilds",property("axisGlyphBuilds").toULongLong()+1);
        setProperty("axisTitleStorage",QStringLiteral("gpu-atlas"));setProperty("axisTitleTexts",cached_label_texts.mid(cached_title_offset,3));
        if(core::perf::enabled.load(std::memory_order_relaxed))setProperty("labelAtlasRebuilds",property("labelAtlasRebuilds").toULongLong()+1);
    }
    void prepareAtlasLayout(){
        const QFontMetrics metrics(font());label_ascent=metrics.ascent();cached_label_sizes.clear();
        atlas_cell={256,std::max(32,metrics.height()+8)};
        for(int index=0;index<cached_label_texts.size();++index){
            const QSizeF extent(metrics.horizontalAdvance(cached_label_texts[index])+(index>=cached_title_offset?16:0),metrics.height()+(index>=cached_title_offset?8:0));
            cached_label_sizes.append(extent);atlas_cell.setWidth(std::max(atlas_cell.width(),int(std::ceil(extent.width()))+2));
        }
    }
    void paintAtlasGlyph(QPainter& painter,const SurfaceFrame& frame,int index){
        const QPointF origin((index%3)*atlas_cell.width(),(index/3)*atlas_cell.height());
        if(index>=cached_title_offset){
            painter.setRenderHint(QPainter::Antialiasing);painter.setBrush(frame.background);painter.setPen(QPen(frame.ink,1));
            painter.drawRoundedRect(QRectF(origin+QPointF(.5,.5),cached_label_sizes[index]-QSizeF(1,1)),3,3);
            painter.drawText(origin+QPointF(8,label_ascent+4),cached_label_texts[index]);
        }else painter.drawText(origin+QPointF(0,label_ascent+1),cached_label_texts[index]);
    }
    void paintAtlasImage(QImage& image,const SurfaceFrame& frame){
        QPainter painter(&image);painter.setFont(font());painter.setPen(frame.ink);
        for(int index=0;index<cached_label_texts.size();++index)paintAtlasGlyph(painter,frame,index);
        painter.end();
    }
    void prepareLabelAtlas(const SurfaceFrame& frame){
        const auto signature=cached_label_texts.join('|')+font().toString()+frame.ink.name()+frame.background.name();
        if(signature==label_signature && label_dpr==devicePixelRatioF())return;
        core::perf::Scope trace(core::perf::Stage::SpectrogramAtlas);describeAtlasBuild();prepareAtlasLayout();
        const auto dpr=devicePixelRatioF();const auto rows=(cached_label_texts.size()+2)/3;
        QImage image(qRound(3*atlas_cell.width()*dpr),qRound(rows*atlas_cell.height()*dpr),QImage::Format_RGBA8888);
        image.setDevicePixelRatio(dpr);image.fill(Qt::transparent);paintAtlasImage(image,frame);
        label_texture=std::make_unique<QOpenGLTexture>(image.flipped(Qt::Vertical));
        label_texture->setMinificationFilter(QOpenGLTexture::Linear);label_texture->setMagnificationFilter(QOpenGLTexture::Linear);
        label_texture->setWrapMode(QOpenGLTexture::ClampToEdge);label_signature=signature;label_dpr=dpr;
    }
    struct AxisLabelFrame {
        const SurfaceFrame& surface;
        QVector<QRectF> occupied;
        int rows=0,titlesDrawn=0,point=0;
        AxisLabelFrame(const SurfaceFrame& frame,int width,int height,int atlasRows):surface(frame),occupied{QRectF(width-76,46,76,140)},rows(atlasRows){Q_UNUSED(height);}
    };
    std::optional<QRectF> projectedLabelBounds(const AxisLabelFrame& labels,int index,QVector3D anchor,QVector2D offset,float angle) const{
        const auto projected=labels.surface.projection*labels.surface.view*QVector4D(anchor,1);
        if(projected.w()<=0)return {};
        QTransform rotation;rotation.rotate(-angle*180/3.141592653589793);
        auto bounds=rotation.mapRect(QRectF(QPointF{},cached_label_sizes[index]));
        bounds.translate((projected.x()/projected.w()+1)*width()/2+offset.x(),(1-projected.y()/projected.w())*height()/2+offset.y()-label_ascent-1);
        if(!QRectF(rect()).adjusted(4,4,-4,-4).contains(bounds))return {};
        return bounds;
    }
    void drawAxisLabel(AxisLabelFrame& labels,int index,QVector3D anchor,QVector2D offset,float angle=0){
        const auto bounds=projectedLabelBounds(labels,index,anchor,offset,angle);if(!bounds)return;
        for(const auto& occupied:std::as_const(labels.occupied))if(bounds->adjusted(-3,-3,3,3).intersects(occupied))return;
        labels.occupied.append(bounds->adjusted(-3,-3,3,3));if(index>=cached_title_offset)++labels.titlesDrawn;
        label_program->setUniformValue("anchor",anchor);label_program->setUniformValue("angle",angle);
        label_program->setUniformValue("offset",offset-QVector2D(0,label_ascent+1));
        label_program->setUniformValue("region",QVector4D((index%3)/3.f,1-float(index/3+1)/labels.rows,1/3.f,1.f/labels.rows));
        glDrawArrays(GL_TRIANGLES,0,6);
    }
    void drawAxisTitles(AxisLabelFrame& labels){
        const auto& frame=labels.surface;const auto& scene=frame.scene;
        const std::array<QVector3D,3> anchors{{{0,-1.f,frame.wallZ*(scene.scaleZ+.4f)},{frame.wallX*(scene.scaleX+.4f),-1.f,0},{-frame.wallX*(scene.scaleX+.15f),1.f,frame.wallZ*(scene.scaleZ+.15f)}}};
        for(int axis=0;axis<3;++axis){
            const auto index=cached_title_offset+axis;
            drawAxisLabel(labels,index,anchors[axis],{-float(cached_label_sizes[index].width())/2,float(axis==2?-8:label_ascent+24)});
        }
    }
    void drawAxisTick(AxisLabelFrame& labels,int axis,int tick){
        const auto& frame=labels.surface;const auto& scene=frame.scene;
        const auto fraction=float(tick)/cached_sections[axis],position=-1+2*fraction;const auto index=cached_label_offsets[axis]+tick;
        if(axis==0){
            if(!std::isfinite(cached_time_indices[tick]) || (tick && cached_time_indices[tick]==cached_time_indices[0]))return;
            const auto x=scene.point(float(cached_time_indices[tick]),scene.zMin,scene.yMin).x();
            drawAxisLabel(labels,index,{x,-1.f,frame.wallZ*(scene.scaleZ+.25f)},{3,14},.523599f*frame.wallX);
        }else if(axis==1){
            if(scene.zMax>scene.zMin || tick==0)drawAxisLabel(labels,index,{frame.wallX*(scene.scaleX+.25f),-1.f,scene.zMax>scene.zMin?-position*scene.scaleZ:0.f},{3,14},-.523599f*frame.wallZ);
        }else drawAxisLabel(labels,index,{-frame.wallX*(scene.scaleX+.1f),-1+2*fraction,frame.wallZ*(scene.scaleZ+.1f)},{5,0});
    }
    void drawAxisTicks(AxisLabelFrame& labels){
        for(int axis=0;axis<3;++axis){
            std::vector<int> order{0,cached_sections[axis]};for(int tick=1;tick<cached_sections[axis];++tick)order.push_back(tick);
            for(int tick:order)drawAxisTick(labels,axis,tick);
        }
    }
    void bindLabelProgram(AxisLabelFrame& labels){
        const auto& frame=labels.surface;label_program->bind();label_texture->bind(0);label_buffer.bind();
        label_program->setUniformValue("mvp",frame.projection*frame.view);label_program->setUniformValue("viewport",QVector2D(width(),height()));
        label_program->setUniformValue("cellSize",QVector2D(atlas_cell.width(),atlas_cell.height()));label_program->setUniformValue("atlas",0);
        labels.point=label_program->attributeLocation("point");label_program->enableAttributeArray(labels.point);label_program->setAttributeBuffer(labels.point,GL_FLOAT,0,2);
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);
    }
    void releaseLabels(const AxisLabelFrame& labels){
        glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);glDisable(GL_BLEND);label_program->disableAttributeArray(labels.point);
        label_buffer.release();label_texture->release();label_program->release();
    }

    void drawLabels(SurfaceFrame& frame){
        const auto started=core::perf::now();recordLabelDiagnostics();prepareLabelAtlas(frame);
        AxisLabelFrame labels(frame,width(),height(),(cached_label_texts.size()+2)/3);
        if(data.empty() || (selection && selected.x()>=0))labels.occupied.append(QRectF(12,8,std::max(1,width()-24),data.empty()?64:36));
        if(label_program->isLinked() && label_texture){bindLabelProgram(labels);drawAxisTitles(labels);drawAxisTicks(labels);releaseLabels(labels);}
        setProperty("axisTitlesDrawn",labels.titlesDrawn);
        core::perf::record(core::perf::Stage::SpectrogramLabels,started,core::perf::now()-started);
    }
    QString readoutLabel(const SurfaceFrame& frame,double value) const{
        return axisReadoutLabel(value,frame.axisLocale,frame.precision);
    }
    void drawOverlayStatus(SurfaceOverlayCommands& painter,const SurfaceFrame& frame){
        if(data.empty())painter.drawText(QRect(12,12,std::max(1,width()-24),60),Qt::AlignLeft|Qt::TextWordWrap,ui::text("Waiting for data. Check the source and connections in Data Flow."));
        const auto spacing=(axes.time_coordinates || axes.column_coordinates)?ui::text("Indexed spacing; coordinates shown in labels"):QString{};
        const auto status=projection_status+(spacing.isEmpty()?QString{}:(projection_status.isEmpty()?QString{}:QStringLiteral(" · "))+spacing);
        if(!status.isEmpty())painter.drawText(QRect(12,std::max(12,height()-44),std::max(1,width()-24),32),Qt::AlignLeft|Qt::AlignBottom,
            QFontMetrics(painter.font()).elidedText(status,Qt::ElideRight,std::max(1,width()-24)));
        Q_UNUSED(frame);
    }
    QGradientStops displayGradient(const SurfaceFrame& frame) const{
        auto stops=frame.stops;
        if(frame.scene.reverseAmplitude){for(auto& stop:stops)stop.first=1-stop.first;std::reverse(stops.begin(),stops.end());}
        return stops;
    }
    void drawLegend(SurfaceOverlayCommands& painter,const SurfaceFrame& frame,const QGradientStops& stops){
        QLinearGradient legend(0,174,0,54);legend.setStops(stops);const auto x=width()-64;
        painter.fillRect(QRect(x,54,14,120),legend);painter.drawRect(QRect(x,54,14,120));
        std::vector<double> ticks;for(int tick=0;tick<=4;++tick)ticks.push_back(frame.scene.signalTick(tick/4.f));
        const auto labels=axisTickLabels(ticks,frame.axisLocale,frame.precision);
        for(int tick=0;tick<=4;++tick)painter.drawText(x+20,178-tick*30,labels[tick]);
    }
    bool hasSelection() const{
        return !data.empty() && selection && selected.x()>=0 && selected.y()>=0 && size_t(selected.y())<data.size() &&
            size_t(selected.x())<std::min(historySize(),data[size_t(selected.y())].size());
    }
    void drawSelectionReadout(SurfaceOverlayCommands& painter,const SurfaceFrame& frame){
        const auto readout=spectrogramSelectionRows(readoutLabel(frame,axes.age(selected.x())),readoutLabel(frame,axes.column(selected.y())),
            readoutLabel(frame,sample(selected.y(),selected.x())),axes.time_coordinates,axes.column_title_key,axes.value_title_key,axes.age_axis,axes.time_title_key);
        painter.save();const auto layout=spectrogramReadoutLayout(readout,painter.font(),size());painter.setFont(layout.font);painter.setPen(frame.ink);
        for(int line=0;line<layout.lines.size();++line)painter.drawText(layout.baselines[line],layout.lines[line]);
        painter.restore();setProperty("selectionReadoutRows",readout);setProperty("selectionReadoutLines",layout.lines);setProperty("selectionReadoutRect",layout.bounds.toAlignedRect());
    }
    static void paintSelectionMarker(SurfaceOverlayCommands& painter,const QPointF& center){
        painter.save();painter.setRenderHint(QPainter::Antialiasing);painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::black,5));painter.drawEllipse(center,8,8);painter.setPen(QPen(Qt::white,2));painter.drawEllipse(center,8,8);
        painter.setBrush(QColor(255,210,0));painter.setPen(QPen(Qt::black,1));painter.drawEllipse(center,3,3);painter.restore();
    }
    void drawSelectionMarker(SurfaceOverlayCommands& painter,const SurfaceFrame& frame){
        const auto projected=projectedSample(selected.x(),selected.y(),frame.scene,frame.projection*frame.view);setProperty("selectionMarkerVisible",false);
        if(!projected.visible)return;
        paintSelectionMarker(painter,projected.point);setProperty("selectionMarkerVisible",rect().contains(projected.point.toPoint()));
        setProperty("selectionMarkerPosition",projected.point);
    }
    QVector4D sliceAxisPoint(const SurfaceFrame& frame,bool timeSlice,int index) const{
        return frame.projection*frame.view*QVector4D(frame.scene.point(timeSlice?selected.x():index,timeSlice?index:selected.y(),frame.scene.signalTick(0)),1);
    }
    struct SliceFrame {
        const SurfaceFrame& surface;
        QRectF panel,plot;
        QColor ink;
        QPen pen;
        bool timeSlice=false,reverse=false;
        int begin=0,end=0,leftIndex=0,rightIndex=0;
        SliceFrame(SpectrogramRenderer& owner,const SurfaceFrame& frame):surface(frame),panel(owner.sliceOverlay.rect(owner.size())),
            plot(spectrogramSlicePlot(panel)),ink(spectrogramTextColor(frame.background)),pen(ink),timeSlice(owner.selection==2){
            begin=int(std::ceil(timeSlice?frame.scene.zMin:frame.scene.xMin));
            end=std::min(int(timeSlice?owner.bandSize():owner.historySize()),timeSlice?int(std::floor(frame.scene.zMax))+1:std::min(int(frame.filled),int(std::floor(frame.scene.xMax))+1));
            reverse=end>begin && spectrogramSliceReversed(owner.sliceAxisPoint(frame,timeSlice,begin),owner.sliceAxisPoint(frame,timeSlice,end-1));
            leftIndex=reverse?end-1:begin;rightIndex=reverse?begin:end-1;pen.setCosmetic(true);
        }
    };
    double sliceCoordinate(const SliceFrame& slice,int index) const{return slice.timeSlice?axes.column(index):axes.age(index);}
    float sliceValue(const SliceFrame& slice,int index) const{return slice.timeSlice?sample(index,selected.x()):sample(selected.y(),index);}
    void describeSlice(const SliceFrame& slice){
        setProperty("slicePanelRect",slice.panel);setProperty("sliceEndpointIndices",QPoint(slice.leftIndex,slice.rightIndex));
        setProperty("sliceEndpointCoordinates",QPointF(sliceCoordinate(slice,slice.leftIndex),sliceCoordinate(slice,slice.rightIndex)));
        if(slice.end>slice.begin)setProperty("sliceEndpointValues",QPointF(sliceValue(slice,slice.leftIndex),sliceValue(slice,slice.rightIndex)));
    }
    QPolygonF slicePolygon(const SliceFrame& slice) const{
        QPolygonF section;section<<slice.plot.bottomLeft();
        for(int column=0;column<slice.end-slice.begin;++column){
            const auto index=slice.reverse?slice.end-1-column:slice.begin+column;const auto value=sliceValue(slice,index);
            section<<QPointF(slice.plot.left()+slice.plot.width()*column/std::max(1,slice.end-slice.begin-1),
                slice.plot.bottom()-slice.plot.height()*std::clamp(slice.surface.scene.verticalFraction(value),0.f,1.f));
        }
        section<<slice.plot.bottomRight();return section;
    }
    void drawSliceSection(SurfaceOverlayCommands& painter,const SliceFrame& slice,const QGradientStops& stops){
        QLinearGradient fill(0,slice.plot.bottom(),0,slice.plot.top());fill.setStops(stops);
        const auto section=slicePolygon(slice);painter.save();painter.setClipRect(slice.plot);painter.setBrush(fill);painter.setPen(slice.pen);
        painter.drawPolygon(section);painter.restore();painter.setBrush(Qt::NoBrush);painter.setPen(slice.pen);painter.drawRect(slice.plot);
    }
    void drawSliceAxes(SurfaceOverlayCommands& painter,const SliceFrame& slice){
        painter.setPen(slice.ink);const auto& frame=slice.surface;
        painter.drawText(QPointF(slice.panel.left()+8,slice.plot.top()+5),readoutLabel(frame,frame.scene.signalTick(1)));
        painter.drawText(QPointF(slice.panel.left()+8,slice.plot.bottom()),readoutLabel(frame,frame.scene.signalTick(0)));
        painter.drawText(QRectF(slice.plot.left(),slice.plot.bottom()+2,slice.plot.width()/2,18),Qt::AlignLeft|Qt::AlignTop,readoutLabel(frame,sliceCoordinate(slice,slice.leftIndex)));
        painter.drawText(QRectF(slice.plot.center().x(),slice.plot.bottom()+2,slice.plot.width()/2,18),Qt::AlignRight|Qt::AlignTop,readoutLabel(frame,sliceCoordinate(slice,slice.rightIndex)));
    }
    void drawSliceTitles(SurfaceOverlayCommands& painter,const SliceFrame& slice){
        painter.drawText(QRectF(slice.plot.left(),slice.panel.bottom()-20,slice.plot.width(),16),Qt::AlignCenter,
            ui::text((slice.timeSlice?axes.column_title_key:axes.time_title_key).toUtf8().constData()));
        painter.save();painter.translate(slice.panel.left()+14,slice.plot.center().y());painter.rotate(-90);
        painter.drawText(QRectF(-slice.plot.height()/2,-10,slice.plot.height(),16),Qt::AlignCenter,ui::text(axes.value_title_key.toUtf8().constData()));painter.restore();
    }
    void drawSlice(SurfaceOverlayCommands& painter,const SurfaceFrame& frame,const QGradientStops& stops){
        const SliceFrame slice(*this,frame);painter.save();painter.setClipRect(slice.panel);describeSlice(slice);
        painter.fillRect(slice.panel,frame.background);painter.setPen(slice.pen);painter.drawRect(slice.panel.adjusted(.5,.5,-.5,-.5));
        painter.drawText(slice.panel.adjusted(8,3,-8,-3),Qt::AlignTop,ui::text(slice.timeSlice?"Time Slice":"Column Slice"));
        drawSliceSection(painter,slice,stops);drawSliceAxes(painter,slice);drawSliceTitles(painter,slice);
        painter.setBrush(Qt::NoBrush);painter.setPen(slice.pen);painter.drawRect(slice.panel.adjusted(.5,.5,-.5,-.5));
        setProperty("sliceSampleCount",std::max(0,slice.end-slice.begin));painter.restore();
    }
    void drawSelection(SurfaceOverlayCommands& painter,const SurfaceFrame& frame,const QGradientStops& stops){
        setProperty("selectedAge",selected.x());setProperty("selectedBand",selected.y());setProperty("selectedValue",sample(selected.y(),selected.x()));
        drawSelectionReadout(painter,frame);drawSelectionMarker(painter,frame);
        if(selection==2 || selection==3)drawSlice(painter,frame,stops);
    }
    void clearSelectionOverlay(){
        setProperty("sliceSampleCount",0);setProperty("selectionMarkerVisible",false);setProperty("selectionReadoutRows",QStringList{});
        setProperty("selectionReadoutLines",QStringList{});setProperty("selectionReadoutRect",QRect{});
    }

    void drawOverlay(SurfaceFrame& frame){
        const auto started=core::perf::now();glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
        if(selection<2)setProperty("sliceSampleCount",0);
        SurfaceOverlayCommands painter(font());painter.setPen(frame.ink);drawOverlayStatus(painter,frame);
        const auto stops=displayGradient(frame);drawLegend(painter,frame,stops);
        if(hasSelection())drawSelection(painter,frame,stops);else clearSelectionOverlay();
        painter.end();setProperty("overlayCommands",painter.commands);
        core::perf::record(core::perf::Stage::SpectrogramOverlay,started,core::perf::now()-started);
    }

    void clearSurface(const QColor& background){
        glClearColor(background.redF(), background.greenF(), background.blueF(), 1);
        // QPainter changes GL state; restore the state required by our next pass.
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthFunc(GL_LESS);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

    }
    void initializeGpuTimer(){
        if(gpu_attempted)return;gpu_attempted=true;
        const auto version=context()->format().version();
        const bool supported=!context()->isOpenGLES() && (version>=std::pair(3,3) || context()->hasExtension("GL_ARB_timer_query") || context()->hasExtension("GL_EXT_timer_query"));
        if(supported){gpu_query=std::make_unique<QOpenGLTimerQuery>();if(!gpu_query->create())gpu_query.reset();}
        setProperty("gpuTimerAvailable",bool(gpu_query));
    }

    bool beginGpuTimer(){
        if(!core::perf::enabled)return false;initializeGpuTimer();
        if(gpu_query && gpu_pending && gpu_query->isResultAvailable()){
            core::perf::duration(core::perf::Stage::SpectrogramGpu,gpu_query->waitForResult());gpu_pending=false;
        }
        if(!gpu_query || gpu_pending)return false;
        gpu_query->begin();return true;
    }
    bool beginFrame(){
        const auto frameStart=core::perf::now();
        if(last_frame)core::perf::duration(core::perf::Stage::SpectrogramFrameGap,frameStart-last_frame);
        last_frame=frameStart;
        if(prepare)prepare();
        return program && program->isLinked();
    }
    void completeFrame(){
        last = clock.elapsed();
        ++frames;
        setProperty("renderedFrames", property("renderedFrames").toULongLong() + 1);
        if (last - start >= 1000) {
            rate = frames * 1000.0 / (last - start);
            frames = 0;
            start = last;
        }

    }

    void renderFramePasses(SurfaceFrame& frame){
        clearSurface(frame.background);const auto gpuActive=beginGpuTimer();
        drawSurface(frame);drawWalls(frame);drawLabels(frame);drawOverlay(frame);
        if(gpuActive){gpu_query->end();gpu_pending=true;}
        completeFrame();
    }

    void paintGL(){
        if(!render_visible)return;
        core::perf::Scope trace(core::perf::Stage::SpectrogramRender);if(!beginFrame())return;
        const auto background=options.value("background_mode")=="custom"?QColor(options.value("background_color").toString()):palette().color(QPalette::Window);
        const auto ink=spectrogramTextColor(background);setProperty("sceneBackgroundColor",background);setProperty("sceneWallColor",background);
        RestoreNewest newest(*this);if(!prepareMesh(newest))return;
        SurfaceFrame frame(*this,background,ink);renderFramePasses(frame);
    }
};

namespace {
class SurfaceFramebufferRenderer final : public QQuickFramebufferObject::Renderer {
    struct FeedbackMailbox {QMutex mutex;QVariantMap latest;bool queued=false;};
    std::shared_ptr<FeedbackMailbox> mailbox=std::make_shared<FeedbackMailbox>();
    SpectrogramRenderer core;
    QPointer<SpectrogramGraph> graph;
    static void deliverFeedback(const QPointer<SpectrogramGraph>& target,
                                const std::shared_ptr<FeedbackMailbox>& delivery) {
        QVariantMap feedback;
        {QMutexLocker lock(&delivery->mutex);feedback=std::move(delivery->latest);delivery->queued=false;}
        if(target)target->accept_render_feedback(feedback);
    }
public:
    void synchronize(QQuickFramebufferObject *object) override {
        auto* item=static_cast<SurfaceQuickItem*>(object);
        auto* source=dynamic_cast<SpectrogramRenderer*>(item->backend());
        if(!source)return;
        graph=qobject_cast<SpectrogramGraph*>(source->parent());
        core.synchronizeFrom(*source);
    }
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
        QOpenGLFramebufferObjectFormat format;format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size,format);
    }
    void queueFeedback(){
        if(!graph)return;
        // At most one queued delivery and one latest immutable result.
        bool post=false;
        {QMutexLocker lock(&mailbox->mutex);mailbox->latest=core.feedback();post=!mailbox->queued;mailbox->queued=true;}
        if(!post)return;
        const auto delivery=mailbox;const QPointer<SpectrogramGraph> target=graph;
        QMetaObject::invokeMethod(graph,[target,delivery]{deliverFeedback(target,delivery);},Qt::QueuedConnection);
    }
    void render() override {
        if(!framebufferObject())return;
        QQuickOpenGLUtils::resetOpenGLState();
        core.renderFrame(framebufferObject()->handle(),framebufferObject()->size());
        QQuickOpenGLUtils::resetOpenGLState();
        queueFeedback();
    }
};
}
SurfaceQuickItem::SurfaceQuickItem(QQuickItem *parent):QQuickFramebufferObject(parent){setMirrorVertically(true);}
void SurfaceQuickItem::setBackend(QObject* value){if(_backend==value)return;_backend=value;emit backendChanged();update();}
QQuickFramebufferObject::Renderer* SurfaceQuickItem::createRenderer() const{return new SurfaceFramebufferRenderer;}

SpectrogramGraph::SpectrogramGraph(QWidget *parent) : QWidget(parent) {
    ui::quick::configure();
    _surface_settings=SurfaceSettings::defaults();
    _camera_timer.setInterval(17);
    _camera_timer.setTimerType(Qt::PreciseTimer);
    connect(&_camera_timer, &QTimer::timeout, this, [this] { _move_camera(); });
}
SpectrogramGraph::~SpectrogramGraph() {
    _stop_camera();
    if(m_spectrogram_graph){
        m_spectrogram_graph->setRenderVisible(false);
        m_spectrogram_graph->canvas=nullptr;
    }
    // Destroy QML bindings while both their graphModel context object and
    // backend state are still alive. QObject child cleanup would otherwise
    // happen after the SpectrogramGraph/model part has been destroyed.
    auto* view=_quick_view;
    _quick_view=nullptr;_quick_item=nullptr;m_container=nullptr;
    if(view){view->removeEventFilter(this);delete view;}
    delete m_spectrogram_graph;
    m_spectrogram_graph=nullptr;
}
QVariantMap SpectrogramGraph::axis_layout_telemetry() const {
    QVariantMap result;
    if(!m_spectrogram_graph)return result;
    const auto propertyNames=m_spectrogram_graph->dynamicPropertyNames();
    for(const auto& key:propertyNames)
        if(key!="overlayCommands")result.insert(QString::fromUtf8(key),m_spectrogram_graph->property(key.constData()));
    result.insert(QStringLiteral("paintCount"),m_spectrogram_graph->property("renderedFrames"));
    return result;
}
qreal SpectrogramGraph::display_fps() const {
    return _render_visible && m_spectrogram_graph && isVisible() ? m_spectrogram_graph->property("displayFps").toReal() : 0;
}
void SpectrogramGraph::create_surface_model(){
    m_spectrogram_graph=new SpectrogramRenderer(this);
    m_spectrogram_graph->history_capacity=_history_capacity;m_spectrogram_graph->configured_bands=_history_bands;
    m_spectrogram_graph->axes=_axis_metadata;m_spectrogram_graph->projection_status=_projection_status;m_spectrogram_graph->options=surface_settings();
    m_spectrogram_graph->colors=ui::Theme::instance().palette();m_spectrogram_graph->labelFont=font();m_spectrogram_graph->language=ui::Language::instance().code();
    m_spectrogram_graph->setRenderVisible(_render_visible);m_spectrogram_graph->setRenderFps(_render_fps);
}
void SpectrogramGraph::create_surface_view(const QSize& minimum,const QSize& maximum){
    static const int registered=qmlRegisterType<SurfaceQuickItem>("Visualization.Surface",1,0,"SurfaceCanvas");
    static const int overlayRegistered=qmlRegisterType<SurfaceOverlayItem>("Visualization.Surface",1,0,"SurfaceOverlay");
    Q_UNUSED(registered);Q_UNUSED(overlayRegistered);
    _quick_view=new QQuickWidget(this);_quick_view->setObjectName("spectrogramQuickView");
    _quick_view->setMinimumSize(minimum);_quick_view->setMaximumSize(maximum);m_container=_quick_view;
}
bool SpectrogramGraph::bind_surface_canvas(){
    ui::quick::initialize(_quick_view,this,QUrl("qrc:/surface3d/SurfaceView.qml"));
    _quick_item=_quick_view->rootObject()?_quick_view->rootObject()->findChild<SurfaceQuickItem*>("spectrogramContainer"):nullptr;
    if(!_quick_item){delete m_spectrogram_graph;m_spectrogram_graph=nullptr;return false;}
    _quick_item->setBackend(m_spectrogram_graph);m_spectrogram_graph->canvas=_quick_item;
    desktop_input::attach_touchpad(_quick_view,[this](QPoint point){
        return _render_visible && _quick_item && point.x()<_quick_item->width() &&
            !(m_spectrogram_graph->selection>=2 && m_spectrogram_graph->property("sliceSampleCount").toInt()>0 && m_spectrogram_graph->sliceOverlay.rect(m_spectrogram_graph->size()).contains(point));
    });
    _quick_view->installEventFilter(this);return true;
}
void SpectrogramGraph::connect_surface_changes(){
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{
        m_spectrogram_graph->colors=ui::Theme::instance().palette();m_spectrogram_graph->labelFont=font();m_spectrogram_graph->requestUpdate();emit quick_state_changed();
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{
        m_spectrogram_graph->language=ui::Language::instance().code();m_spectrogram_graph->requestUpdate();emit quick_state_changed();
    });
    connect(this,&SpectrogramGraph::surface_settings_updated,this,[this]{emit quick_state_changed();});
    connect(this,&SpectrogramGraph::y_range_changed,this,[this]{emit quick_state_changed();});
}
void SpectrogramGraph::restore_surface_state(){
    const auto orientation=QQuaternion::fromEulerAngles(-45,45,0);
    const auto framing=float(std::max(.1,std::min(1.,.625*_quick_item->width()/std::max(1.,_quick_item->height()))));
    const auto position=orientation.rotatedVector({0,0,6.f/(1.55f*framing)});
    _default_camera_state={{"version",2},{"position",QJsonArray{position.x(),position.y(),position.z()}},{"quaternion",QJsonArray{orientation.scalar(),orientation.x(),orientation.y(),orientation.z()}}};
    set_camera_state(_camera_state.isEmpty()?_default_camera_state:_camera_state);
    set_presentation_state(_presentation_state);set_surface_settings(_surface_settings);m_spectrogram_graph->setData(_spectrogram);
    quick_viewport(_quick_item->width(),_quick_item->height());
}

bool SpectrogramGraph::initialize(const QSize& minimum,const QSize& maximum){
    if(_initialization_attempted)return m_spectrogram_graph;_initialization_attempted=true;
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    create_surface_model();create_surface_view(minimum,maximum);layout->addWidget(_quick_view);
    if(!bind_surface_canvas())return false;
    connect_surface_changes();restore_surface_state();return true;
}
SpectrogramSceneGeometry SpectrogramGraph::projected_scene_geometry() const {
    if(m_spectrogram_graph)return m_spectrogram_graph->geometry();
    const auto values=presentation_state().value("ranges").toArray();
    int ranges[4]={0,100,0,100};
    if(values.size()==4)for(int axis=0;axis<2;++axis) {
        ranges[axis*2]=std::clamp(values[axis*2].toInt(),0,99);
        ranges[axis*2+1]=std::clamp(values[axis*2+1].toInt(),ranges[axis*2]+1,100);
    }
    return spectrogramSceneGeometry(_history_capacity?_history_capacity:projected_history_frames(),
        _history_bands?_history_bands:projected_history_bands(),ranges,surface_settings(),
        {float(_amplitude_range.range().first),float(_amplitude_range.range().second)});
}
void SpectrogramGraph::set_paused(bool value) {
    if(value==_pause)return;
    _presentation_state["pause"]=value;
    if(value) {
        _pending_projection={_history_capacity,_history_bands,_source_axis_metadata,_projection_status,_spectrogram};
        _pause=true;
        emit quick_state_changed();
        return;
    }
    _pause=false;
    auto pending=std::move(_pending_projection);
    _pending_projection={};
    set_history_dimensions(pending.capacity,pending.bands);
    set_axis_metadata(pending.axes);
    set_projection_status(pending.status);
    update_snapshot(std::move(pending.data));
    emit quick_state_changed();
}
void SpectrogramGraph::sync_history_dimensions_to_renderer(size_t capacity,size_t bands){
    if(!m_spectrogram_graph)return;
    m_spectrogram_graph->history_capacity=capacity;
    m_spectrogram_graph->configured_bands=bands;
    if(m_spectrogram_graph->selected.x()>=int(capacity) || m_spectrogram_graph->selected.y()>=int(bands))
        m_spectrogram_graph->selected={-1,-1};
    m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
}
void SpectrogramGraph::set_history_dimensions(size_t capacity,size_t bands) {
    if(!capacity || !bands || capacity>16384 || bands>4096 || capacity*bands>1048576) return;
    if(_pause){_pending_projection.capacity=capacity;_pending_projection.bands=bands;return;}
    if(capacity==_history_capacity && bands==_history_bands) return;
    _history_capacity=capacity;_history_bands=bands;
    sync_history_dimensions_to_renderer(capacity,bands);
    emit quick_state_changed();
}
void SpectrogramGraph::set_value_scale(const ValueDisplayScale& scale) {
    auto metadata=_pause?_pending_projection.axes:_source_axis_metadata;
    // Keep the source descriptor/coordinates of the pending snapshot, not the
    // frozen display, when changing presentation while paused.
    metadata.value_scale.mode=scale.mode;metadata.value_scale.reference=scale.reference;
    metadata.value_scale.floor=scale.floor;metadata.value_scale.override_factor=scale.override_factor;
    if(_pause){_pending_projection.axes=std::move(metadata);return;}
    if(metadata!=_source_axis_metadata)set_axis_metadata(metadata);
}
bool SpectrogramGraph::valid_axis_metadata(const SpectrogramAxisMetadata& value){
    if(value.ages.size()>16384 || value.row_indices.size()>16384 || value.columns.size()>4096 || value.time_title_key.size()>128 || value.column_title_key.size()>128 || value.value_title_key.size()>128 || value.time_index_title_key.size()>128 || value.column_index_title_key.size()>128)return false;
    const auto valid=[](const std::vector<double>& coordinates,bool eitherDirection=false){
        const bool descending=eitherDirection && coordinates.size()>1 && coordinates[1]<coordinates[0];
        for(size_t index=0;index<coordinates.size();++index)if(!std::isfinite(coordinates[index]) || (index && (descending?coordinates[index]>=coordinates[index-1]:coordinates[index]<=coordinates[index-1])))return false;
        return true;
    };
    return valid(value.ages,!value.age_axis) && valid(value.row_indices,true) && valid(value.columns) && !(value.age_axis && value.time_coordinates && !value.ages.empty() && value.ages[0]!=0);
}
void SpectrogramGraph::sync_axis_metadata(bool scaleChanged){
    if(scaleChanged)_amplitude_range.reset();
    if(m_spectrogram_graph){
        m_spectrogram_graph->axes=_axis_metadata;if(scaleChanged)m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
    }
    if(scaleChanged)_observe_amplitude_range();
}
bool SpectrogramGraph::apply_axis_metadata(const SpectrogramAxisMetadata& value){
    const bool scaleChanged=_source_axis_metadata.value_scale!=value.value_scale,sourceChanged=_source_axis_metadata!=value;
    _source_axis_metadata=value;const auto displayed=value.displayed(_frequency_axis_mode,_time_axis_mode);
    if(_axis_metadata==displayed){if(sourceChanged)emit axis_metadata_changed();return true;}
    _axis_metadata=displayed;sync_axis_metadata(scaleChanged);
    if(sourceChanged)emit axis_metadata_changed();emit quick_state_changed();return true;
}

bool SpectrogramGraph::set_axis_metadata(const SpectrogramAxisMetadata& value){
    if(!valid_axis_metadata(value))return false;
    if(_pause){_pending_projection.axes=value;return true;}
    return apply_axis_metadata(value);
}
void SpectrogramGraph::set_axis_display_modes(const QString& frequencyMode,const QString& timeMode) {
    _frequency_axis_mode=frequencyMode=="positions"?"positions":"coordinates";
    _time_axis_mode=timeMode=="positions"?"positions":"coordinates";
    _axis_metadata=_source_axis_metadata.displayed(_frequency_axis_mode,_time_axis_mode);
    if(m_spectrogram_graph){m_spectrogram_graph->axes=_axis_metadata;m_spectrogram_graph->requestUpdate();}
    emit quick_state_changed();
}
void SpectrogramGraph::set_projection_status(const QString& value) {
    if(_pause){_pending_projection.status=value;return;}
    if(_projection_status==value)return;
    _projection_status=value;
    if(m_spectrogram_graph){m_spectrogram_graph->projection_status=value;m_spectrogram_graph->requestUpdate();}
}
void SpectrogramGraph::set_render_fps(int fps) {
    _render_fps=std::clamp(fps,1,1000);
    const int cameraInterval=(1000+_render_fps-1)/_render_fps;
    if(_camera_timer.interval()!=cameraInterval) _camera_timer.setInterval(cameraInterval);
    if(m_spectrogram_graph) m_spectrogram_graph->setRenderFps(_render_fps);
}
void SpectrogramGraph::set_render_visible(bool visible) {
    if(_render_visible==visible) return;
    _render_visible=visible;
    if(!visible) _stop_camera();
    if(m_spectrogram_graph) m_spectrogram_graph->setRenderVisible(visible);
    setUpdatesEnabled(visible);
}
QPair<float,float> SpectrogramGraph::y_range() const {
    const auto options=surface_settings();
    if(options["y_range_mode"]=="manual")return {float(options["y_min"].toDouble()),float(options["y_max"].toDouble(1))};
    const auto extent=_amplitude_range.range();
    return {extent.first,extent.second};
}
void SpectrogramGraph::set_y_range(float minimum,float maximum) {
    if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < -1e6f || maximum > 1e6f || maximum<=minimum) return;
    auto options=surface_settings();options["y_range_mode"]="manual";options["y_min"]=minimum;options["y_max"]=maximum;
    set_surface_settings(options);
}
QJsonObject SpectrogramGraph::surface_settings() const { return SurfaceSettings::normalized(_surface_settings); }
void SpectrogramGraph::apply_surface_options(){
    if(!m_spectrogram_graph)return;
    const auto previous=m_spectrogram_graph->options;const bool changed=previous!=_surface_settings;
    for(const auto* key:{"reverse_time","reverse_bands","reverse_amplitude","y_range_mode","y_min","y_max","scene_aspect_mode","scene_time_aspect","scene_band_aspect"})
        if(previous[key]!=_surface_settings[key]){m_spectrogram_graph->upload=true;break;}
    m_spectrogram_graph->options=_surface_settings;m_spectrogram_graph->gradient=_surface_settings["gradient"]=="black_yellow"?0:1;
    if(changed)m_spectrogram_graph->requestUpdate();
}

void SpectrogramGraph::set_surface_settings(const QJsonObject& effective){
    const auto previous=y_range();const bool wasManual=surface_settings().value("y_range_mode")=="manual";
    _surface_settings=SurfaceSettings::normalized(effective);
    if(wasManual && _surface_settings["y_range_mode"]!="manual")_amplitude_range.reset();
    _observe_amplitude_range();apply_surface_options();
    const auto current=y_range();if(previous!=current)emit y_range_changed(current.first,current.second);
    emit surface_settings_updated(_surface_settings);
}
void SpectrogramGraph::update_frame_history(const std::vector<std::vector<float>> &history) {
    update_snapshot(NumericMatrixSnapshot::take(history).transposed());
}
void SpectrogramGraph::update(const std::vector<std::vector<float>> &value) {
    update_snapshot(NumericMatrixSnapshot::take(value));
}
void SpectrogramGraph::publish_snapshot(size_t length,size_t previousFrames,QPair<float,float> previousRange){
    ++_history_projection_revision;
    if(!_history_capacity && !_spectrogram.empty() && m_spectrogram_graph){
        m_spectrogram_graph->history_capacity=std::max(m_spectrogram_graph->history_capacity,length);m_spectrogram_graph->configured_bands=_spectrogram.size();
    }
    if(m_spectrogram_graph)m_spectrogram_graph->setData(_spectrogram);
    const auto current=y_range();if(previousRange!=current)emit y_range_changed(current.first,current.second);
    if(previousFrames!=projected_history_frames())emit quick_state_changed();
}

void SpectrogramGraph::accept_snapshot(NumericMatrixSnapshot snapshot,size_t length){
    const auto previousFrames=projected_history_frames();
    const auto previousRange=y_range();
    const bool changed=_spectrogram!=snapshot;
    _spectrogram=std::move(snapshot);
    const bool rangeChanged=_observe_amplitude_range();
    if(!changed && !rangeChanged){
        if(m_spectrogram_graph)m_spectrogram_graph->setData(_spectrogram,false);
        return;
    }
    publish_snapshot(length,previousFrames,previousRange);
}
void SpectrogramGraph::update_snapshot(NumericMatrixSnapshot snapshot){
    core::perf::Scope trace(core::perf::Stage::SpectrogramData);const auto length=snapshot.empty()?size_t{0}:snapshot.front().size();
    if(snapshot.size()>4096 || length>16384)snapshot.clear();
    if(_pause){_pending_projection.data=std::move(snapshot);return;}
    accept_snapshot(std::move(snapshot),length);
}
QPair<double,double> SpectrogramGraph::observed_amplitude_bounds() const {
    double low=0,high=0;
    for(size_t row=0;row<_spectrogram.size();++row)
        for(size_t column=0;column<_spectrogram[row].size();++column){
            const double value=_axis_metadata.value_scale.map(_spectrogram[row][column]);
            low=std::min(low,value);high=std::max(high,value);
        }
    return {low,high};
}
void SpectrogramGraph::sync_automatic_amplitude_range(bool changed){
    if(!m_spectrogram_graph)return;
    const auto bounds=_amplitude_range.range();
    m_spectrogram_graph->automatic_range={float(bounds.first),float(bounds.second)};
    if(changed){m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();}
}
bool SpectrogramGraph::_observe_amplitude_range() {
    if(_pause || surface_settings().value("y_range_mode")=="manual" || _spectrogram.empty())return false;
    const auto bounds=observed_amplitude_bounds();
    const bool changed=_amplitude_range.observe(bounds.first,bounds.second,amplitude_clock(),
        surface_settings().value("auto_range_interval_seconds").toDouble(10));
    sync_automatic_amplitude_range(changed);
    return changed;
}
QJsonObject SpectrogramGraph::camera_state() const {
    auto state=_camera_state;
    state["version"]=2;
    state["position"]=QJsonArray{_position.x(),_position.y(),_position.z()};
    state["quaternion"]=QJsonArray{_orientation.scalar(),_orientation.x(),_orientation.y(),_orientation.z()};
    return state;
}
bool SpectrogramGraph::finite_camera_array(const QJsonArray& array,int size){
    if(array.size()!=size)return false;
    for(const auto& value:array)if(!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble())>1000000)return false;
    return true;
}
bool SpectrogramGraph::restore_camera_pose(const QJsonObject& state){
    const auto position=state.value("position").toArray(),quaternion=state.value("quaternion").toArray();
    if(!finite_camera_array(position,3) || !finite_camera_array(quaternion,4))return false;
    _position={float(position[0].toDouble()),float(position[1].toDouble()),float(position[2].toDouble())};
    const QQuaternion candidate(quaternion[0].toDouble(),quaternion[1].toDouble(),quaternion[2].toDouble(),quaternion[3].toDouble());
    if(candidate.lengthSquared()>1e-8)_orientation=CameraGeometry::uprightLook(candidate.normalized());
    return true;
}
void SpectrogramGraph::restore_orbit_state(const QJsonObject& state){
    if(!state.contains("zoom"))return;
    const float yaw=state.value("horizontal").toDouble(),pitch=state.value("vertical").toDouble(30),zoom=state.value("zoom").toDouble(100);
    if(!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(zoom))return;
    _orientation=CameraGeometry::uprightLook(QQuaternion::fromEulerAngles(-pitch,-yaw,0));
    _position=_orientation.rotatedVector({0,0,std::clamp(300/std::max(1.f,zoom),.01f,10.f)});
    const auto target=state.value("target").toArray();
    if(finite_camera_array(target,3))_position+=QVector3D(target[0].toDouble(),target[1].toDouble(),target[2].toDouble());
}
void SpectrogramGraph::sync_camera_model(){
    if(!m_spectrogram_graph)return;
    m_spectrogram_graph->position=_position;m_spectrogram_graph->orientation=_orientation;m_spectrogram_graph->requestUpdate();
}

void SpectrogramGraph::set_camera_state(const QJsonObject& state){
    if(state.isEmpty())return;
    if(!restore_camera_pose(state))restore_orbit_state(state);
    if(_position.length()>10)_position=_position.normalized()*10;
    _movement_speed=CameraControls::movementSpeed(_camera_controls);sync_camera_model();_camera_state=state;_camera_state=camera_state();
}
void SpectrogramGraph::_camera_changed() {
    core::perf::Scope profile(core::perf::Stage::CameraMove);
    if (m_spectrogram_graph) {
        m_spectrogram_graph->position = _position;
        m_spectrogram_graph->orientation = _orientation;
        m_spectrogram_graph->requestUpdate();
    }
    _camera_state = camera_state();
    emit camera_changed(_camera_state);
}
void SpectrogramGraph::_move_camera() {
    if (!_render_visible || !m_spectrogram_graph || !isVisible()) {
        _stop_camera();
        return;
    }
    const float dt = std::min(_camera_clock.restart() / 1000.f, .05f);
    QVector3D move(_camera_keys.contains("right") - _camera_keys.contains("left"),
                   _camera_keys.contains("up") - _camera_keys.contains("down"),
                   _camera_keys.contains("backward") - _camera_keys.contains("forward"));
    if(move.isNull())return;
    if (move.lengthSquared() > 1)
        move.normalize();
    _position = CameraGeometry::translatedPosition(_position,CameraGeometry::horizontalMovement(_orientation,move) *
        dt * _movement_speed * (_camera_keys.contains("fast") ? 2.5f : 1.f));
    _camera_changed();
}
void SpectrogramGraph::_stop_camera() {
    _camera_keys.clear();
    _held_camera_keys.clear();
    _camera_timer.stop();
}
bool SpectrogramGraph::held_camera_action_matches(const QString& action,const QKeyEvent& event) const {
    if(action=="reset")return false;
    const auto bindings=_camera_controls.value(action).toArray();
    for(const auto& binding:bindings)
        if(CameraControls::matches(binding.toObject(),event,action=="fast"))return true;
    return false;
}
void SpectrogramGraph::_refresh_camera_keys(Qt::KeyboardModifiers modifiers) {
    _camera_keys.clear();
    for(const auto& held:std::as_const(_held_camera_keys)) {
        const QKeyEvent event(QEvent::KeyPress,held.second,modifiers,quint32(held.first),0,0);
        for(auto it=_camera_controls.begin();it!=_camera_controls.end();++it)
            if(held_camera_action_matches(it.key(),event))_camera_keys.insert(it.key());
    }
    if(_camera_keys.isEmpty())_camera_timer.stop();
    else if(!_camera_timer.isActive()){_camera_clock.start();_camera_timer.start();}
}
void SpectrogramGraph::set_camera_controls(const QJsonObject &controls) {
    _stop_camera();
    _camera_controls = CameraControls::normalized(controls);
    _movement_speed=CameraControls::movementSpeed(_camera_controls);
}
void SpectrogramGraph::reset_camera() {
    _stop_camera();
    set_camera_state(_default_camera_state);
    _camera_changed();
}
void SpectrogramGraph::set_side_panel_ratio(float ratio) {
    _side_panel_ratio = std::isfinite(ratio) ? std::clamp(ratio, .01f, .99f) : .25f;
    emit quick_state_changed();
}
void SpectrogramGraph::set_controls_embedded(bool embedded) {
    if(_controls_embedded==embedded)return;
    _controls_embedded=embedded;
    emit quick_state_changed();
}
QVector3D SpectrogramGraph::camera_translation(float yaw, float side, float up, float horizontal_ratio, float aspect) {
    const float ratio = std::max(horizontal_ratio, .001f);
    QVector3D direction(std::cos(yaw) * side, up, -std::sin(yaw) * side);
    if (direction.lengthSquared() > 1)
        direction.normalize();
    return {direction.x() / std::min(1.f, ratio), direction.y() * aspect, direction.z() / std::min(1.f, 1 / ratio)};
}
void SpectrogramGraph::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if(m_spectrogram_graph){m_spectrogram_graph->visible=true;m_spectrogram_graph->setRenderVisible(_render_visible);}
    if (m_spectrogram_graph)
        m_spectrogram_graph->requestUpdate();
}
void SpectrogramGraph::hideEvent(QHideEvent *event) {
    _stop_camera();
    if(m_spectrogram_graph){m_spectrogram_graph->visible=false;m_spectrogram_graph->setRenderVisible(false);}
    QWidget::hideEvent(event);
}
void SpectrogramGraph::refresh_input_state(QEvent* event){
    if(event->type()==QEvent::PaletteChange || event->type()==QEvent::FontChange){
        m_spectrogram_graph->colors=ui::Theme::instance().palette();m_spectrogram_graph->labelFont=font();
        m_spectrogram_graph->requestUpdate();
    }
    if(event->type()==QEvent::Resize || event->type()==QEvent::Hide){
        m_spectrogram_graph->sliceOverlay.end();m_spectrogram_graph->endPointDrag();
    }
    if(event->type()==QEvent::FocusOut || event->type()==QEvent::WindowDeactivate){
        _stop_camera();m_spectrogram_graph->sliceOverlay.end();m_spectrogram_graph->endPointDrag();
    }
}
bool SpectrogramGraph::pointer_press(QEvent* event){
    const auto* mouse=static_cast<QMouseEvent*>(event);_mouse_position=mouse->position().toPoint();m_container->setFocus();
    if(mouse->button()==Qt::LeftButton){
        m_spectrogram_graph->endPointDrag();
        if(!(mouse->buttons()&(Qt::RightButton|Qt::MiddleButton)) && !m_spectrogram_graph->beginSliceDrag(mouse->position()))
            m_spectrogram_graph->beginPointDrag(mouse->position());
    }
    if(mouse->button()==Qt::RightButton || mouse->button()==Qt::MiddleButton){
        m_spectrogram_graph->endPointDrag();m_spectrogram_graph->sliceOverlay.end();
    }
    return mouse->button()==Qt::LeftButton || mouse->button()==Qt::RightButton || mouse->button()==Qt::MiddleButton;
}
bool SpectrogramGraph::pointer_camera_move(QEvent* event){
    const auto* mouse=static_cast<QMouseEvent*>(event);
    if(!(mouse->buttons()&(Qt::RightButton|Qt::MiddleButton)))return false;
    const auto delta=mouse->position().toPoint()-_mouse_position;_mouse_position=mouse->position().toPoint();
    if(delta.isNull())return true;
    if(mouse->buttons()&Qt::MiddleButton)_position+=_orientation.rotatedVector(QVector3D(-delta.x(),delta.y(),0))*.005f;
    else _orientation=CameraGeometry::uprightLook(_orientation,-delta.x()*.3f,-delta.y()*.3f);
    _camera_changed();return true;
}
bool SpectrogramGraph::pointer_move(QEvent* event){
    const auto* mouse=static_cast<QMouseEvent*>(event);
    if(!(mouse->buttons()&Qt::LeftButton))m_spectrogram_graph->endPointDrag();
    if(m_spectrogram_graph->sliceOverlay.active && (mouse->buttons()&Qt::LeftButton)){
        m_spectrogram_graph->moveSlice(mouse->position());return true;
    }
    if(m_spectrogram_graph->point_drag && (mouse->buttons()&Qt::LeftButton) && !(mouse->buttons()&(Qt::RightButton|Qt::MiddleButton))){
        m_spectrogram_graph->pick(mouse->position(),true);return true;
    }
    if(mouse->buttons()==Qt::NoButton){
        const bool slice=m_spectrogram_graph->selection>=2 && m_spectrogram_graph->property("sliceSampleCount").toInt()>0;
        m_container->setCursor(slice?m_spectrogram_graph->sliceOverlay.cursor(m_spectrogram_graph->size(),mouse->position()):Qt::ArrowCursor);
    }
    return pointer_camera_move(event);
}
bool SpectrogramGraph::pointer_release(QEvent* event){
    if(static_cast<QMouseEvent*>(event)->button()!=Qt::LeftButton)return false;
    const bool captured=m_spectrogram_graph->point_drag || m_spectrogram_graph->sliceOverlay.active;
    if(m_spectrogram_graph->sliceOverlay.active){m_spectrogram_graph->sliceOverlay.end();emit presentation_changed(presentation_state());}
    m_spectrogram_graph->endPointDrag();return captured;
}
bool SpectrogramGraph::pointer_event(QEvent* event){
    switch(event->type()){
    case QEvent::MouseButtonPress:return pointer_press(event);
    case QEvent::MouseMove:return pointer_move(event);
    case QEvent::MouseButtonRelease:return pointer_release(event);
    case QEvent::Wheel:return camera_wheel(event);
    default:return false;
    }
}
void SpectrogramGraph::orbit_camera(double pixels){
    const auto rotation=QQuaternion::fromAxisAndAngle({0,1,0},float(-pixels*.3));
    _position=rotation.rotatedVector(_position);_orientation=(rotation*_orientation).normalized();
}
void SpectrogramGraph::zoom_camera(double amount){
    _position+=_orientation.rotatedVector({0,0,float(-amount)});
    if(_position.length()>10)_position=_position.normalized()*10;
}
bool SpectrogramGraph::camera_gesture(QEvent* event){
    const auto* gesture=static_cast<QNativeGestureEvent*>(event);
    if(gesture->gestureType()==Qt::PanNativeGesture){orbit_camera(gesture->delta().x());zoom_camera(gesture->delta().y()/150.0);}
    else if(gesture->gestureType()==Qt::ZoomNativeGesture){
        const auto ratio=std::clamp(1+gesture->value(),.1,10.0);_position/=float(ratio);
        if(_position.length()>10)_position=_position.normalized()*10;
    }else return false;
    _camera_changed();return true;
}
bool SpectrogramGraph::camera_wheel(QEvent* event){
    const auto* wheel=static_cast<QWheelEvent*>(event);
    if(wheel->pixelDelta().isNull() && wheel->angleDelta().isNull())return true;
    if(!wheel->pixelDelta().isNull()){orbit_camera(wheel->pixelDelta().x());zoom_camera(wheel->pixelDelta().y()/150.0);}
    else {orbit_camera(wheel->angleDelta().x()/4.0);zoom_camera(wheel->angleDelta().y()/600.0);}
    _camera_changed();return true;
}
bool SpectrogramGraph::apply_camera_binding(const QString& action,const QKeyEvent& key,int scan,int identity){
    if(key.isAutoRepeat())return true;
    if(key.type()==QEvent::KeyPress){
        if(action=="reset")reset_camera();
        else {_held_camera_keys[identity]={scan,key.key()};_refresh_camera_keys(key.modifiers());}
    }
    return true;
}
std::optional<bool> SpectrogramGraph::camera_key_binding(QEvent* event,int scan,int identity){
    const auto* key=static_cast<QKeyEvent*>(event);
    if(key->modifiers()&(Qt::AltModifier|Qt::MetaModifier))return false;
    for(auto it=_camera_controls.begin();it!=_camera_controls.end();++it) {
        const auto bindings=it.value().toArray();
        for(const auto& binding:bindings)
            if(CameraControls::matches(binding.toObject(),*key,it.key()=="fast"))
                return apply_camera_binding(it.key(),*key,scan,identity);
    }
    return {};
}
std::optional<bool> SpectrogramGraph::camera_control_key(QEvent* event,int scan,int identity){
    const auto* key=static_cast<QKeyEvent*>(event);
    auto modifiers=key->modifiers();
    if(key->key()==Qt::Key_Control){
        if(event->type()==QEvent::KeyPress)modifiers|=Qt::ControlModifier;else modifiers&=~Qt::ControlModifier;
        _refresh_camera_keys(modifiers);return false;
    }
    if(event->type()==QEvent::KeyRelease && !key->isAutoRepeat() && _held_camera_keys.remove(identity)){
        _refresh_camera_keys(modifiers);return true;
    }
    return camera_key_binding(event,scan,identity);
}
std::optional<bool> SpectrogramGraph::camera_key(QEvent* event){
    const auto* key=static_cast<QKeyEvent*>(event);
    if(key->key()==Qt::Key_Escape){_stop_camera();return true;}
    const bool resetChord=key->modifiers()==Qt::AltModifier && (CameraControls::scan(*key)==19 || CameraControls::logical(key->key())==Qt::Key_R);
    if(resetChord){if(event->type()==QEvent::KeyPress && !key->isAutoRepeat())reset_camera();return true;}
    const auto scan=CameraControls::scan(*key),identity=scan?scan:CameraControls::logical(key->key())+0x10000;
    return camera_control_key(event,scan,identity);
}

std::optional<bool> SpectrogramGraph::camera_control_event(QEvent* event){
    const bool focused=!_quick_item || _quick_item->hasActiveFocus();
    if(event->type()==QEvent::NativeGesture && focused && camera_gesture(event))return true;
    if((event->type()==QEvent::KeyPress || event->type()==QEvent::KeyRelease) && focused){
        return camera_key(event);
    }
    return {};
}
bool SpectrogramGraph::eventFilter(QObject* watched,QEvent* event){
    if(!m_spectrogram_graph)return QWidget::eventFilter(watched,event);
    refresh_input_state(event);
    if(!_render_visible)return QWidget::eventFilter(watched,event);
    if(watched==_quick_item && pointer_event(event))return true;
    if(const auto handled=camera_control_event(event))return *handled;
    return QWidget::eventFilter(watched,event);
}

QJsonObject SpectrogramGraph::presentation_state() const {
    auto state = _presentation_state;
    if (m_spectrogram_graph) {
        state["slice_position"] = QJsonArray{m_spectrogram_graph->sliceOverlay.position.x(),m_spectrogram_graph->sliceOverlay.position.y()};
        if(m_spectrogram_graph->sliceOverlay.preferredSize.isValid())
            state["slice_size"] = QJsonArray{m_spectrogram_graph->sliceOverlay.preferredSize.width(),m_spectrogram_graph->sliceOverlay.preferredSize.height()};
        state["ranges"] = QJsonArray{m_spectrogram_graph->ranges[0], m_spectrogram_graph->ranges[1],
                                     m_spectrogram_graph->ranges[2], m_spectrogram_graph->ranges[3]};
        state["selection"] = m_spectrogram_graph->selection;
        state["pause"] = _pause;
    }
    return state;
}
void SpectrogramGraph::restore_slice_geometry(const QJsonObject& state){
    auto& overlay=m_spectrogram_graph->sliceOverlay;overlay.end();overlay.preferredSize={};
    const auto size=state.value("slice_size").toArray();
    if(size.size()==2){
        const auto width=size[0].toDouble(),height=size[1].toDouble();
        if(std::isfinite(width) && std::isfinite(height) && width>0 && height>0)overlay.preferredSize={std::clamp(width,180.0,16384.0),std::clamp(height,120.0,16384.0)};
    }
    const auto position=state.value("slice_position").toArray();
    if(position.size()==2){
        const auto x=position[0].toDouble(1),y=position[1].toDouble(1);
        if(std::isfinite(x) && std::isfinite(y))overlay.position={std::clamp(x,0.0,1.0),std::clamp(y,0.0,1.0)};
    }
}
void SpectrogramGraph::restore_surface_ranges(const QJsonObject& state){
    const auto ranges=state.value("ranges").toArray();
    for(int axis=0;axis<2;++axis){
        const auto low=ranges.size()==4?std::clamp(ranges[axis*2].toInt(),0,99):0;
        auto high=ranges.size()==4?std::clamp(ranges[axis*2+1].toInt(),1,100):100;
        if(low>=high)high=low+1;
        m_spectrogram_graph->ranges[axis*2]=low;m_spectrogram_graph->ranges[axis*2+1]=high;
    }
}

void SpectrogramGraph::set_presentation_state(const QJsonObject& state){
    _presentation_state=state;set_paused(state.value("pause").toBool(false));
    if(!m_spectrogram_graph)return;
    restore_slice_geometry(state);restore_surface_ranges(state);
    m_spectrogram_graph->selection=std::clamp(state.value("selection").toInt(1),0,3);
    set_paused(state.value("pause").toBool(false));m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
    emit surface_settings_updated(surface_settings());emit quick_state_changed();
}


QVariantList SpectrogramGraph::horizontal_automatic_ranges(const QJsonArray& ranges) const {
    const auto automatic=_presentation_state.value("ranges_auto").toArray();
    QVariantList result;
    for(int axis=0;axis<2;++axis)result.append(automatic.size()==2 ? automatic[axis].toBool(true) :
        ranges.size()!=4 || (ranges[axis*2].toInt()==0 && ranges[axis*2+1].toInt()==100));
    return result;
}
QVariantMap SpectrogramGraph::quick_state() const {
    auto result=presentation_state().toVariantMap();
    result["surface"]=surface_settings().toVariantMap();result["pause"]=_pause;
    result["panelRatio"]=_side_panel_ratio;
    const auto ranges=presentation_state().value("ranges").toArray();
    result["horizontalAutomatic"]=horizontal_automatic_ranges(ranges);
    const auto range=y_range();result["yMinimum"]=range.first;result["yMaximum"]=range.second;
    result["timeTitle"]=ui::text(_axis_metadata.time_title_key.toUtf8().constData());
    result["bandTitle"]=ui::text(_axis_metadata.column_title_key.toUtf8().constData());
    result["valueTitle"]=ui::text(_axis_metadata.value_title_key.toUtf8().constData());
    result["historyFrames"]=qulonglong(projected_history_frames());result["historyCapacity"]=qulonglong(_history_capacity);
    return result;
}
void SpectrogramGraph::apply_quick_selection(const QVariant& value){
    m_spectrogram_graph->endPointDrag();
    m_spectrogram_graph->selection=std::clamp(value.toInt(),0,3);
    m_spectrogram_graph->requestUpdate();
    emit presentation_changed(presentation_state());
}
void SpectrogramGraph::apply_quick_panel_ratio(const QVariant& value){
    const auto before=_side_panel_ratio;set_side_panel_ratio(value.toFloat());
    if(before!=_side_panel_ratio)emit side_panel_resized(_side_panel_ratio);
}
bool SpectrogramGraph::apply_quick_control(const QString& action,const QVariant& value){
    if(action=="pause"){set_paused(value.toBool());emit presentation_changed(presentation_state());}
    else if(action=="selection" && m_spectrogram_graph)apply_quick_selection(value);
    else if(action=="resetCamera")reset_camera();
    else if(action=="panelRatio")apply_quick_panel_ratio(value);
    else return false;
    return true;
}
bool SpectrogramGraph::apply_quick_gradient(const QVariant& value){
    if(value.toString()=="custom"){emit edit_color_scale_requested();return false;}
    const QStringList presets{"green_red","black_yellow","viridis","ice_fire","rainbow"};
    if(!presets.contains(value.toString()))return false;
    auto options=surface_settings();options["gradient"]=value.toString();options["gradient_stops"]=QJsonArray{};
    set_surface_settings(options);emit surface_settings_changed(surface_settings());return true;
}

void SpectrogramGraph::quick_action(const QString& action,const QVariant& value){
    if(action=="cursorReset"){if(m_container)m_container->unsetCursor();return;}
    if(action=="gradient"){if(!apply_quick_gradient(value))return;}
    else if(!apply_quick_control(action,value) && surface_settings().contains(action)){
        auto options=surface_settings();options[action]=QJsonValue::fromVariant(value);set_surface_settings(options);emit surface_settings_changed(surface_settings());
    }
    emit quick_state_changed();
}
void SpectrogramGraph::set_signal_range(double low,double high,bool automatic){
    auto options=surface_settings();options["y_range_mode"]=automatic?"auto":"manual";options["y_min"]=low;options["y_max"]=high;
    set_surface_settings(options);emit surface_settings_changed(surface_settings());
}
void SpectrogramGraph::set_index_range(int axis,double low,double high,bool automatic){
    const auto index=axis==0?0:2;auto automaticAxes=_presentation_state.value("ranges_auto").toArray();
    if(automaticAxes.size()!=2)automaticAxes=QJsonArray{true,true};
    automaticAxes[index/2]=automatic;_presentation_state["ranges_auto"]=automaticAxes;
    const auto minimum=automatic?0:std::clamp(qRound(low),0,99),maximum=automatic?100:std::clamp(qRound(high),minimum+1,100);
    m_spectrogram_graph->ranges[index]=minimum;m_spectrogram_graph->ranges[index+1]=maximum;
    m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();emit presentation_changed(presentation_state());
}

void SpectrogramGraph::quick_range(int axis,double low,double high,bool automatic){
    if(!std::isfinite(low) || !std::isfinite(high) || !m_spectrogram_graph)return;
    if(axis==1)set_signal_range(low,high,automatic);else if(axis==0 || axis==2)set_index_range(axis,low,high,automatic);
    emit quick_state_changed();
}
void SpectrogramGraph::quick_viewport(double width,double height) {
    if(!m_spectrogram_graph||!std::isfinite(width)||!std::isfinite(height))return;
    const QSize viewport(std::max(1,qRound(width)),std::max(1,qRound(height)));
    const auto dpr=_quick_view?_quick_view->devicePixelRatioF():devicePixelRatioF();
    if(m_spectrogram_graph->viewport==viewport && m_spectrogram_graph->dpr==dpr)return;
    m_spectrogram_graph->viewport=viewport;
    m_spectrogram_graph->dpr=dpr;
    m_spectrogram_graph->requestUpdate();
}
void SpectrogramGraph::quick_pointer(int type,double x,double y,int button,int buttons,int modifiers){
    if(!_quick_item)return;
    const auto eventType=type==0?QEvent::MouseButtonPress:type==1?QEvent::MouseMove:QEvent::MouseButtonRelease;
    QMouseEvent event(eventType,QPointF(x,y),QPointF(x,y),Qt::MouseButton(button),Qt::MouseButtons(buttons),Qt::KeyboardModifiers(modifiers));
    eventFilter(_quick_item,&event);
}
void SpectrogramGraph::quick_wheel(double horizontal,double vertical,bool pixels){
    if(!_quick_item||!std::isfinite(horizontal)||!std::isfinite(vertical))return;
    const QPoint delta(qRound(horizontal),qRound(vertical));
    QWheelEvent event(QPointF{},QPointF{},pixels?delta:QPoint{},pixels?QPoint{}:delta,Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    eventFilter(_quick_item,&event);
}
void SpectrogramGraph::quick_key(bool pressed,int key,int modifiers,bool repeat){
    QKeyEvent event(pressed?QEvent::KeyPress:QEvent::KeyRelease,key,Qt::KeyboardModifiers(modifiers),QString{},repeat);
    eventFilter(_quick_item,&event);
}
void SpectrogramGraph::quick_cancel_input(){
    _stop_camera();if(m_spectrogram_graph){m_spectrogram_graph->endPointDrag();m_spectrogram_graph->sliceOverlay.end();}
}
void SpectrogramGraph::accept_render_feedback(const QVariantMap& feedback){
    core::perf::Scope trace(core::perf::Stage::QuickSurfaceFeedback);
    if(!m_spectrogram_graph)return;
    const auto previousError=m_spectrogram_graph->property("shadowError").toString();
    m_spectrogram_graph->frameConsumed(feedback);
    const QVariantMap nextOverlay{{"commands",feedback.value("overlayCommands")}};
    if(_overlay!=nextOverlay){
        _overlay=nextOverlay;
        emit overlay_changed();
    }
    const auto error=feedback.value("shadowError").toString();if(!error.isEmpty()&&error!=previousError)emit surface_settings_error(error);
}
