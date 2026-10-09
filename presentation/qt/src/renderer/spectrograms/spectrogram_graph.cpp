#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/camera_geometry.h"
#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include <QtDataVisualization/Q3DTheme>
#include "visualization/ui_text.h"
#include "visualization/surface_settings.h"
#include "visualization/axis_tick_labels.h"
#include "visualization/scene_aspect_control.h"
#include "visualization/axis_control_widget.h"
#include "visualization/auto_range_interval_control.h"
#include "visualization/spectrograms/spectrogram_selection.h"
#include "core/performance.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QJsonArray>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QGroupBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QSignalBlocker>
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
#include <cstring>

class SpectrogramRenderer final : public QOpenGLWidget, protected QOpenGLFunctions {
  public:
    explicit SpectrogramRenderer(QWidget *parent) : QOpenGLWidget(parent), buffer(QOpenGLBuffer::VertexBuffer) {
        setObjectName("spectrogramContainer");
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(240, 180);
        timer.setSingleShot(true);
        timer.setTimerType(Qt::PreciseTimer);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (dirty && render_visible && isVisible() && !frame_pending) {
                dirty = false;
                frame_pending=true;
                core::perf::duration(core::perf::Stage::SpectrogramSchedule, core::perf::now()-request_time);
                QOpenGLWidget::update();
            } else if (!dirty || !render_visible || !isVisible())
                timer.stop();
        });
    }
    ~SpectrogramRenderer() override {
        timer.stop();
        if (context())
            disconnect(context(), nullptr, this, nullptr);
        cleanup();
    }
    QVector3D position{0, 1.6f, 2.8f};
    QQuaternion orientation = QQuaternion::fromEulerAngles(-30, 0, 0);
    std::function<void()> prepare;
    std::function<void(const QString &)> shadow_error;
    NumericMatrixSnapshot data;
    bool upload = true, dirty = false;
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
    float sample(size_t band,size_t age) const {return spectrogramHistorySample(data,band,age,historySize());}
    QGradientStops gradientStops() const {return SurfaceSettings::gradientStops(options);}
    void requestUpdate() {
        if(!dirty) request_time=core::perf::now();
        dirty = true;
        if(!render_visible || !isVisible()) return;
        if (!timer.isActive() && !frame_pending) {
            // Pace against the last actual frame, not a fresh 17-ms delay
            // every time the latest-only mailbox becomes dirty again.
            const auto now=core::perf::now();
            const auto deadline=last_frame+frame_interval;
            timer.start(now>=deadline ? 0 : int((deadline-now+999999)/1000000));
        }
    }
    void setRenderFps(int fps) {
        const auto interval=quint64((1000000000LL+fps-1)/fps);
        if(frame_interval==interval) return;
        frame_interval=interval;timer.stop();
        if(dirty) requestUpdate();
    }
    void setRenderVisible(bool visible) {
        if(render_visible==visible) return;
        render_visible=visible;timer.stop();
        if(!visible){endPointDrag();sliceOverlay.end();}
        dirty|=frame_pending;frame_pending=false;
        rate=0;frames=0;last_frame=0;
        if(clock.isValid()) start=last=clock.elapsed();
        if(visible) {request_time=core::perf::now();requestUpdate();}
    }
    void setData(const NumericMatrixSnapshot &value,bool changed=true) {
        core::perf::Scope handoff(core::perf::Stage::SpectrogramHandoff);
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
    QTimer timer;
    quint64 request_time=0;
    quint64 last_frame=0;
    quint64 frame_interval=16666667;
    bool frame_pending=false;
    GLfloat line_range[2]{1,1};
    std::unique_ptr<QOpenGLTimerQuery> gpu_query;
    bool gpu_pending=false, gpu_attempted=false;
    Q3DTheme theme{Q3DTheme::ThemeEbony};
    QOpenGLBuffer buffer;
    QOpenGLBuffer edge_buffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLBuffer floor_buffer{QOpenGLBuffer::VertexBuffer};
    QString grid_signature;
    QString axis_policy_signature;
    std::unique_ptr<QOpenGLShaderProgram> program, label_program, shadow_program;
    GLuint shadow_framebuffer=0, shadow_texture=0, shadow_color=0;
    bool shadow_attempted=false, shadow_ready=false;
    std::unique_ptr<QOpenGLTexture> label_texture;
    QOpenGLBuffer label_buffer{QOpenGLBuffer::VertexBuffer};
    QString label_signature;
    std::vector<float> vertices;
    int count = 0, capacity = 0, edge_capacity = 0;
    GLenum surface_primitive=GL_TRIANGLES;
    std::vector<GLuint> edge_indices;
    std::vector<QVector3D> face_normals;
    std::vector<std::array<float,6>> sample_vertices;
    QElapsedTimer clock;
    qint64 start = 0, last = 0;
    int frames = 0;
    qreal rate = 0;
    void cleanup() {
        if (!context())
            return;
        makeCurrent();
        // Qt 6.10 destroy() clears the query's context; its destructor still
        // dereferences that context. Let the destructor delete the GL resource
        // once, while the owning widget context is current.
        if(gpu_query) {gpu_query.reset();gpu_pending=false;}
        gpu_attempted=false;
        if(shadow_framebuffer) glDeleteFramebuffers(1,&shadow_framebuffer);
        if(shadow_texture) glDeleteTextures(1,&shadow_texture);
        if(shadow_color) glDeleteRenderbuffers(1,&shadow_color);
        shadow_framebuffer=shadow_texture=shadow_color=0;shadow_attempted=shadow_ready=false;
        shadow_program.reset();
        buffer.destroy();
        edge_buffer.destroy();
        floor_buffer.destroy();
        program.reset();
        label_texture.reset();
        label_program.reset();
        label_buffer.destroy();
        label_signature.clear();
        grid_signature.clear();
        axis_policy_signature.clear();
        doneCurrent();
        capacity = edge_capacity = 0;
        upload = true;
    }
    bool ensureShadowMap() {
        if(shadow_attempted) return shadow_ready;
        shadow_attempted=true;
        auto failed=[&](const QString &reason) {
            setProperty("shadowReady",false);setProperty("shadowError",reason);
            options["shadows"]=false;
            if(shadow_error) shadow_error(reason);
            return false;
        };
        if(context()->isOpenGLES()) return failed("Shadow mapping is unavailable on this graphics backend.");
        shadow_program=std::make_unique<QOpenGLShaderProgram>();
        shadow_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "attribute vec3 vertex; uniform mat4 lightMvp; void main(){gl_Position=lightMvp*vec4(vertex,1.0);}");
        shadow_program->addShaderFromSourceCode(QOpenGLShader::Fragment,"void main(){gl_FragColor=vec4(1.0);}");
        if(!shadow_program->link()) return failed("Cannot initialize the shadow-map shader.");
        glActiveTexture(GL_TEXTURE0);glGenTextures(1,&shadow_texture);glBindTexture(GL_TEXTURE_2D,shadow_texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT16,1024,1024,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1,&shadow_framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,shadow_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,shadow_texture,0);
        // Compatibility profiles require a complete color attachment even
        // though the depth pass masks all color writes.
        glGenRenderbuffers(1,&shadow_color);glBindRenderbuffer(GL_RENDERBUFFER,shadow_color);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA4,1024,1024);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,shadow_color);
        const auto status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());glBindTexture(GL_TEXTURE_2D,0);
        if(status!=GL_FRAMEBUFFER_COMPLETE) return failed("Cannot allocate the shadow depth map on this graphics backend.");
        shadow_ready=true;setProperty("shadowReady",true);return true;
    }
    void renderShadowMap(const QMatrix4x4 &lightMvp) {
        core::perf::Scope shadowTrace(core::perf::Stage::SpectrogramShadow);
        glBindFramebuffer(GL_FRAMEBUFFER,shadow_framebuffer);glViewport(0,0,1024,1024);
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_TRUE);glClear(GL_DEPTH_BUFFER_BIT);
        shadow_program->bind();shadow_program->setUniformValue("lightMvp",lightMvp);
        buffer.bind();const auto point=shadow_program->attributeLocation("vertex");
        shadow_program->enableAttributeArray(point);shadow_program->setAttributeBuffer(point,GL_FLOAT,0,3,6*sizeof(float));
        glDrawArrays(surface_primitive,0,count);
        shadow_program->disableAttributeArray(point);buffer.release();shadow_program->release();
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glBindFramebuffer(GL_FRAMEBUFFER,defaultFramebufferObject());
        glViewport(0,0,qRound(width()*devicePixelRatioF()),qRound(height()*devicePixelRatioF()));
        setProperty("shadowPasses",property("shadowPasses").toULongLong()+1);
    }
    void initializeGL() override {
        initializeOpenGLFunctions();
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE,line_range);
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanup(); }, Qt::DirectConnection);
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
        label_program = std::make_unique<QOpenGLShaderProgram>();
        label_program->addShaderFromSourceCode(
            QOpenGLShader::Vertex,
            "attribute vec2 point; uniform mat4 mvp; uniform vec3 anchor; uniform vec2 offset; "
            "uniform vec2 viewport; uniform float angle; uniform vec4 region; varying vec2 uv; void main(){vec4 p=mvp*vec4(anchor,1.0); "
            "vec2 text=point*vec2(256.0,32.0); text=mat2(cos(angle),-sin(angle),sin(angle),cos(angle))*text; p.xy+=vec2(2.0/viewport.x,-2.0/viewport.y)*(offset+text)*p.w; "
            "p.z-=0.000001*p.w;gl_Position=p;uv=region.xy+vec2(point.x,1.0-point.y)*region.zw;}");
        label_program->addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            "uniform sampler2D atlas; varying vec2 uv; void main(){vec4 c=texture2D(atlas,uv); "
            "if(c.a<0.01)discard;gl_FragColor=c;}");
        setProperty("axisLabelsReady", label_program->link());
        const float label_quad[]{0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
        label_buffer.create();
        label_buffer.bind();
        label_buffer.allocate(label_quad, sizeof(label_quad));
        label_buffer.release();
        buffer.create();
        edge_buffer.create();
        floor_buffer.create();
        upload = true;
        start = last = 0;
        frames = 0;
        clock.start();
    }
    void paintGL() override {
        if(!render_visible) {dirty=true;frame_pending=false;timer.stop();return;}
        core::perf::Scope trace(core::perf::Stage::SpectrogramRender);
        const auto frameStart=core::perf::now();
        if(last_frame) core::perf::duration(core::perf::Stage::SpectrogramFrameGap,frameStart-last_frame);
        last_frame=frameStart;
        // This paint consumes every data/camera change already handed off.
        frame_pending=false;dirty=false;timer.stop();
        if (prepare)
            prepare();
        if (!program || !program->isLinked())
            return;
        theme.setType(palette().color(QPalette::Window).lightness()<128 ? Q3DTheme::ThemeEbony : Q3DTheme::ThemeArmyBlue);
        const auto background=options.value("background_mode")=="custom" ? QColor(options.value("background_color").toString()) :
            palette().color(QPalette::Window);
        theme.setWindowColor(background);theme.setBackgroundColor(background);
        theme.setGridLineColor(palette().color(QPalette::Mid));
        theme.setLabelTextColor(spectrogramTextColor(background));
        setProperty("sceneBackgroundColor",background);setProperty("sceneWallColor",theme.backgroundColor());
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
        if (upload) {
            core::perf::Scope geometryTrace(core::perf::Stage::SpectrogramGeometry);
            const auto meshStart=core::perf::now();
            vertices.clear();
            const size_t bands=bandSize(),history=historySize();
            const auto scene = geometry();
            const size_t b0=size_t(std::ceil(scene.zMin)),b1=std::min(bands,size_t(std::floor(scene.zMax))+1);
            const size_t t0=size_t(std::ceil(scene.xMin)),t1=std::min(history,size_t(std::floor(scene.xMax))+1);
            setProperty("historyCapacity",qulonglong(history));setProperty("historyBands",qulonglong(bands));
            setProperty("validHistoryColumns",qulonglong(data.empty() ? 0 : std::min(history,data.front().size())));
            setProperty("sceneScaleX", scene.scaleX); setProperty("sceneScaleZ", scene.scaleZ);
            const size_t bs=std::max(size_t(1),(b1-b0)/256), ts=std::max(size_t(1),(t1-t0)/256);
            const size_t rows=data.empty() || b1<=b0 ? 0 : (b1-b0+bs-1)/bs;
            const size_t columns=t1<=t0 ? 0 : (t1-t0+ts-1)/ts;
            sample_vertices.resize(rows*columns);
            float peakValue=-std::numeric_limits<float>::infinity();size_t peakFrame=0,peakBand=0;
            for(size_t row=0;row<rows;++row) for(size_t column=0;column<columns;++column) {
                const size_t startBand=std::min(b0+row*bs,b1-1),startAge=std::min(t0+column*ts,t1-1);
                const auto peak=spectrogramPeakSample(data,startBand,std::min(startBand+bs,b1),
                    startAge,std::min(startAge+ts,t1),history);
                const size_t b=peak.band,t=peak.age;
                const float value=peak.value;const auto p=scene.point(t,b,value);
                if(value>peakValue) {peakValue=value;peakFrame=t;peakBand=b;}
                const auto left=t ? t-1 : t, right=std::min(t+1,history-1);
                const auto lower=b ? b-1 : b, upper=std::min(b+1,bands-1);
                const auto dx=scene.point(right,b,sample(b,right))-scene.point(left,b,sample(b,left));
                const auto dz=scene.point(t,upper,sample(upper,t))-scene.point(t,lower,sample(lower,t));
                auto n=QVector3D::crossProduct(dx,dz).normalized();
                if(scene.reverseTime!=scene.reverseBands) n=-n;
                sample_vertices[row*columns+column]={p.x(),p.y(),p.z(),n.x(),n.y(),n.z()};
            }
            setProperty("surfacePeakAge",qulonglong(peakFrame));
            setProperty("surfacePeakFrame",qulonglong(history ? history-1-peakFrame : 0));setProperty("surfacePeakBand",qulonglong(peakBand));
            auto vertex=[&](size_t row,size_t column) {
                const auto &v=sample_vertices[row*columns+column];vertices.insert(vertices.end(),v.begin(),v.end());
            };
            setProperty("firstDisplayedValue",history ? sample(0,history-1) : 0.f);
            setProperty("latestDisplayedValue",sample(0,0));
            surface_primitive=rows>1 && columns>1 ? GL_TRIANGLES :
                rows*columns>1 ? GL_LINES : GL_POINTS;
            if(surface_primitive==GL_TRIANGLES) {
                for(size_t row=0;row+1<rows;++row) for(size_t column=0;column+1<columns;++column) {
                    vertex(row,column);vertex(row+1,column);vertex(row,column+1);
                    vertex(row,column+1);vertex(row+1,column);vertex(row+1,column+1);
                }
            } else if(rows && columns) {
                if(rows==1 && columns==1)vertex(0,0);
                else if(rows==1)for(size_t column=0;column+1<columns;++column){vertex(0,column);vertex(0,column+1);}
                else for(size_t row=0;row+1<rows;++row){vertex(row,0);vertex(row+1,0);}
            }
            setProperty("surfacePrimitive",surface_primitive==GL_TRIANGLES ? "triangles" :
                surface_primitive==GL_LINES ? "lines" : "points");
            core::perf::record(core::perf::Stage::SpectrogramMesh,meshStart,core::perf::now()-meshStart);
            float maximum_height = !vertices.empty() ? -std::numeric_limits<float>::infinity() : 0;
            for (size_t i = 1; i < vertices.size(); i += 6)
                maximum_height = std::max(maximum_height, vertices[i]);
            setProperty("surfaceMaximumHeight", maximum_height);
            count = int(vertices.size() / 6);
            setProperty("vertexCount", count);
            setProperty("geometryUploads", property("geometryUploads").toULongLong() + 1);
            const auto uploadStart=core::perf::now();
            buffer.bind();
            const int bytes = int(vertices.size() * sizeof(float));
            if (bytes > capacity) {
                capacity = bytes;
                buffer.allocate(nullptr, capacity);
            }
            if (bytes)
                buffer.write(0, vertices.data(), bytes);
            if(property("verifyGeometryUpload").toBool()) {
                std::vector<float> readback(vertices.size());
                const bool matched=!bytes || (buffer.read(0,readback.data(),bytes) && std::memcmp(vertices.data(),readback.data(),std::size_t(bytes))==0);
                setProperty("geometryUploadVerified",matched);
            }
            buffer.release();
            core::perf::record(core::perf::Stage::SpectrogramUpload,uploadStart,core::perf::now()-uploadStart);
            const auto creaseStart=core::perf::now();
            // Draw creases rather than a blanket wire grid. Coplanar faces,
            // including sloping planes, must have no internal triangle lines.
            // The two triangles of each regular cell have known neighbors.
            // Retain their normals, rather than allocating a coordinate-keyed
            // tree for every edge on every spectrum frame. Emit exactly the
            // same endpoints, order and 5-degree criterion as the old search.
            edge_indices.clear();
            face_normals.resize(count/3);
            const size_t cellColumns=columns ? columns-1 : 0;
            auto vector = [&](GLuint index) {
                return QVector3D(vertices[index*6],vertices[index*6+1],vertices[index*6+2]);
            };
            auto normal = [&](GLuint triangle) {
                return QVector3D::crossProduct(vector(triangle+1)-vector(triangle),
                                              vector(triangle+2)-vector(triangle)).normalized();
            };
            auto edge = [&](const QVector3D &a,const QVector3D &b,GLuint first,GLuint second) {
                if(QVector3D::dotProduct(a,b)<0.9961947f) edge_indices.insert(edge_indices.end(),{first,second});
            };
            for(GLuint cell=0;surface_primitive==GL_TRIANGLES && cell<GLuint(count/6);++cell) {
                const GLuint vertex=cell*6;
                const auto a=normal(vertex), b=normal(vertex+3);
                face_normals[cell*2]=a;face_normals[cell*2+1]=b;
                if(cellColumns && cell%cellColumns) edge(face_normals[(cell-1)*2+1],a,vertex,vertex+1);
                if(cellColumns && cell>=cellColumns) edge(face_normals[(cell-cellColumns)*2+1],a,vertex+2,vertex);
                edge(a,b,vertex+3,vertex+4);
            }
            core::perf::record(core::perf::Stage::SpectrogramCreases,creaseStart,core::perf::now()-creaseStart);
            const auto edgeUploadStart=core::perf::now();
            edge_buffer.bind();
            const int edge_bytes = int(edge_indices.size() * sizeof(GLuint));
            if (edge_bytes > edge_capacity) {
                edge_capacity = edge_bytes;
                edge_buffer.allocate(nullptr, edge_capacity);
            }
            if (edge_bytes)
                edge_buffer.write(0, edge_indices.data(), edge_bytes);
            edge_buffer.release();
            core::perf::record(core::perf::Stage::SpectrogramUpload,edgeUploadStart,core::perf::now()-edgeUploadStart);
            setProperty("wireframeEdgeCount", int(edge_indices.size() / 2));
            upload = false;
        }
        bool gpuActive=false;
        if(core::perf::enabled) {
            if(!gpu_attempted) {
                gpu_attempted=true;
                const auto version=context()->format().version();
                const bool supported=!context()->isOpenGLES() && (version>=std::pair(3,3) ||
                    context()->hasExtension("GL_ARB_timer_query") || context()->hasExtension("GL_EXT_timer_query"));
                if(supported) {
                    gpu_query=std::make_unique<QOpenGLTimerQuery>();
                    if(!gpu_query->create()) gpu_query.reset();
                }
                setProperty("gpuTimerAvailable",bool(gpu_query));
            }
            if(gpu_query && gpu_pending && gpu_query->isResultAvailable()) {
                core::perf::duration(core::perf::Stage::SpectrogramGpu,gpu_query->waitForResult());gpu_pending=false;
            }
            if(gpu_query && !gpu_pending) {gpu_query->begin();gpuActive=true;}
        }
        const auto lightPosition=orientation.rotatedVector({0,0,9.5f})+QVector3D(0,.5f,0);
        QMatrix4x4 shadowProjection,shadowView;
        // The scene-attached light is ~9.5 units away. A near plane of 3
        // retains precision across the bounded scene (as Qt's native shadow
        // renderer does), rather than letting the receiver bias erase shadows.
        shadowProjection.perspective(60,1,3.f,30);
        shadowView.lookAt(lightPosition,{0,0,0},orientation.rotatedVector({0,1,0}));
        const auto shadowMvp=shadowProjection*shadowView;
        const bool shadows=options["shadows"].toBool() && ensureShadowMap();
        if(shadows) renderShadowMap(shadowMvp);
        setProperty("shadowsActive",shadows);
        const auto surfaceStart=core::perf::now();
        QMatrix4x4 projection;
        projection.perspective(45, float(width()) / std::max(1, height()), 0.005f, 100.0f);
        QMatrix4x4 view;
        view.rotate(orientation.conjugated());
        view.translate(-position);
        program->bind();
        program->setUniformValue("mvp", projection * view);
        program->setUniformValue("eyePosition", position);
        program->setUniformValue("shadowsEnabled",shadows);program->setUniformValue("shadowMvp",shadowMvp);
        program->setUniformValue("shadowMap",1);
        if(shadows) {glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,shadow_texture);glActiveTexture(GL_TEXTURE0);}
        program->setUniformValue("lightPosition",lightPosition);
        const auto lightColor=theme.lightColor();
        program->setUniformValue("lightColor", QVector3D(lightColor.redF(),lightColor.greenF(),lightColor.blueF()));
        program->setUniformValue("lightStrength",theme.lightStrength()*float(options["light_strength"].toDouble(1)));
        program->setUniformValue("ambientStrength",theme.ambientLightStrength()*float(options["ambient_strength"].toDouble(1)));
        program->setUniformValue("lightingEnabled",options["light"].toBool(true));
        program->setUniformValue("brightness",float(options["brightness"].toDouble(1)));
        program->setUniformValue("gradient", gradient);
        const auto stops=gradientStops();std::array<GLfloat,16> offsets{};std::array<QVector3D,16> colors{};
        const int stopCount=std::min(16,int(stops.size()));
        for(int i=0;i<stopCount;++i) {offsets[i]=stops[i].first;const auto color=stops[i].second;
            colors[i]={float(color.redF()),float(color.greenF()),float(color.blueF())};}
        program->setUniformValue("stopCount",stopCount);
        program->setUniformValueArray("stopPositions",offsets.data(),stopCount,1);
        program->setUniformValueArray("stopColors",colors.data(),stopCount);
        program->setUniformValue("gridOpacity",1.f);
        program->setUniformValue("model3d", model3d);
        program->setUniformValue("gridPass", false);
        program->setUniformValue("floorPass", false);
        program->setUniformValue("selection", selection);
        const auto scene = geometry();
        program->setUniformValue("reverseAmplitude",scene.reverseAmplitude);
        const auto selectedPosition = scene.point(selected.x(),selected.y(),0);
        program->setUniformValue("selected", selected.x()<0 ? QVector2D(-100,-100) : QVector2D(selectedPosition.x(),selectedPosition.z()));
        buffer.bind();
        const int location = program->attributeLocation("vertex");
        program->enableAttributeArray(location);
        program->setAttributeBuffer(location, GL_FLOAT, 0, 3, 6 * sizeof(float));
        const int normal_location = program->attributeLocation("normal");
        program->enableAttributeArray(normal_location);
        program->setAttributeBuffer(normal_location, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
        // Offset only the filled pass. Edge positions remain on the exact mesh,
        // with depth testing preventing back-side edges bleeding through peaks.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.f, 1.f);
        glDrawArrays(surface_primitive, 0, count);
        glDisable(GL_POLYGON_OFFSET_FILL);
        program->setUniformValue("gridPass", true);
        const auto meshSetting=options["mesh_color"].toString("auto");
        const QColor meshColor=meshSetting=="auto" ? QColor("#08171c") : QColor(meshSetting);
        program->setUniformValue("gridColor",QVector3D(meshColor.redF(),meshColor.greenF(),meshColor.blueF()));
        const float contrast=float(options["mesh_contrast"].toDouble(1));
        program->setUniformValue("gridOpacity",contrast);
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        const float lineWidth=std::clamp(float(options["mesh_width"].toDouble(1)),line_range[0],line_range[1]);
        glLineWidth(lineWidth);setProperty("meshActualWidth",lineWidth);
        edge_buffer.bind();
        if(options["mesh_grid"].toBool(true)) glDrawElements(GL_LINES, int(edge_indices.size()), GL_UNSIGNED_INT, nullptr);
        edge_buffer.release();
        glDisable(GL_BLEND);glLineWidth(1);program->setUniformValue("gridOpacity",1.f);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        buffer.release();
        core::perf::record(core::perf::Stage::SpectrogramSurface,surfaceStart,core::perf::now()-surfaceStart);
        const auto wallsStart=core::perf::now();
        const float wallX=position.x()>=0 ? 1.f : -1.f, wallZ=position.z()>=0 ? 1.f : -1.f;
        const QLocale axisLocale(ui::Language::instance().code()=="ru" ? "ru_RU" : "en_US");
        const int precision=options["axis_precision"].toInt(0);
        const size_t filled=data.empty()?0:std::min(historySize(),data.front().size());
        const size_t knownAges=axes.time_coordinates?std::min(filled,axes.ages.size()):filled;
        setProperty("validAgeCount",qulonglong(knownAges));
        const auto metrics=QFontMetrics(font());
        std::array<int,3> sections{};
        std::array<std::vector<double>,3> tickValues;
        std::array<QStringList,3> tickLabels;
        std::vector<double> timeIndices;
        for(int axis=0;axis<3;++axis) {
            int candidate=spectrogramAxisSections(size(),{double(metrics.horizontalAdvance("000")),double(metrics.height())},axis);
            if(axis<2) {
                const double extent=axis==0 ? scene.xMax-scene.xMin : scene.zMax-scene.zMin;
                candidate=spectrogramReadableSections(extent,candidate,axis==0?!axes.time_coordinates:!axes.column_coordinates);
            }
            if(axis==2)candidate=std::max(height()>=220?4:1,spectrogramReadableSections(double(scene.yMax)-scene.yMin,candidate,false));
            for(;;) {
                tickValues[axis].clear();
                if(axis==0)timeIndices=spectrogramValidTimeTicks(scene,knownAges,candidate,!axes.time_coordinates);
                for(int tick=0;tick<=candidate;++tick) {
                    const float fraction=float(tick)/candidate;
                    tickValues[axis].push_back(axis==0 ? axes.age(timeIndices[tick]) :
                        axis==1 ? axes.column(scene.bandTick(fraction)) : scene.signalTick(fraction));
                }
                tickLabels[axis]=axisTickLabels(tickValues[axis],axisLocale,precision);
                int widest=0;for(const auto& text:tickLabels[axis])widest=std::max(widest,metrics.horizontalAdvance(text));
                const int fitting=spectrogramAxisSections(size(),{double(widest),double(metrics.height())},axis);
                if(candidate<=fitting || candidate==1)break;
                candidate=std::min(candidate-1,fitting);
                if(axis==0 && !axes.time_coordinates)candidate=spectrogramReadableSections(scene.xMax-scene.xMin,candidate,true);
                if(axis==1 && !axes.column_coordinates)candidate=spectrogramReadableSections(scene.zMax-scene.zMin,candidate,true);
                if(axis==2)candidate=std::max(height()>=220?4:1,spectrogramReadableSections(double(scene.yMax)-scene.yMin,candidate,false));
            }
            sections[axis]=candidate;
        }
        setProperty("timeAxisSections",sections[0]);setProperty("bandAxisSections",sections[1]);
        setProperty("valueAxisSections",sections[2]);
        QString policySignature;
        for(int axis=0;axis<3;++axis) {
            QVariantList values;
            for(double value:tickValues[axis]) {values.append(value);policySignature+=QString::number(value,'g',17)+QLatin1Char('/');}
            policySignature+=QLatin1Char('|');
            setProperty(axis==0?"timeAxisTickValues":axis==1?"bandAxisTickValues":"valueAxisTickValues",values);
        }
        setProperty("timeAxisTitle",axes.time_title_key);setProperty("bandAxisTitle",axes.column_title_key);
        if(axis_policy_signature!=policySignature) {
            axis_policy_signature=policySignature;
            setProperty("axisPolicyRevision",property("axisPolicyRevision").toULongLong()+1);
        }
        setProperty("horizontalGridLines",options["axis_grid"].toBool(true)?sections[2]+1:0);
        setProperty("horizontalGridLinesTotal",options["axis_grid"].toBool(true)?2*(sections[2]+1):0);
        const int floorVertices=2*(sections[0]+sections[1]+2);
        const int wallVertices=2*(sections[0]+sections[1]+2)+4*(sections[2]+1);
        const auto gridSignature=QString("%1/%2/%3/%4/%5/%6/%7/%8/%9").arg(sections[0]).arg(sections[1]).arg(sections[2])
            .arg(scene.scaleX,0,'g',9).arg(scene.scaleZ,0,'g',9).arg(qulonglong(knownAges))
            .arg(scene.xMin,0,'g',9).arg(scene.xMax,0,'g',9).arg(scene.reverseTime);
        const auto timeGridX=[&](int tick) {
            // Known partial physical time covers only real timestamped rows.
            // Grid positions follow those exact label indices, including crop.
            return std::isfinite(timeIndices[tick]) ?
                scene.point(float(timeIndices[tick]),scene.zMin,scene.yMin).x() :
                scene.scaleX*(-1+2.f*tick/sections[0]);
        };
        if(grid_signature!=gridSignature) {
            std::vector<float> floor;
            const auto line=[&](QVector3D a,QVector3D b) {
                floor.insert(floor.end(),{a.x(),a.y(),a.z(),0,1,0,b.x(),b.y(),b.z(),0,1,0});
            };
            const float sx=scene.scaleX+.1f,sz=scene.scaleZ+.1f;
            constexpr float floorY=-1.01f;
            for(int tick=0;tick<=sections[0];++tick) {
                const float x=timeGridX(tick);line({x,floorY,-sz},{x,floorY,sz});
            }
            for(int tick=0;tick<=sections[1];++tick) {
                const float z=scene.scaleZ*(1-2.f*tick/sections[1]);line({-sx,floorY,z},{sx,floorY,z});
            }
            for(int tick=0;tick<=sections[0];++tick) {
                const float x=timeGridX(tick);line({x,-1,-sz},{x,1,-sz});
            }
            for(int tick=0;tick<=sections[1];++tick) {
                const float z=scene.scaleZ*(1-2.f*tick/sections[1]);line({-sx,-1,z},{-sx,1,z});
            }
            for(int tick=0;tick<=sections[2];++tick) {
                const float y=-1+2.f*tick/sections[2];line({-sx,y,-sz},{sx,y,-sz});line({-sx,y,-sz},{-sx,y,sz});
            }
            const auto face=[&](QVector3D a,QVector3D b,QVector3D c,QVector3D d) {
                for(auto p:{a,b,c,a,c,d})floor.insert(floor.end(),{p.x(),p.y(),p.z(),0,1,0});
            };
            face({-sx,floorY,-sz},{sx,floorY,-sz},{sx,floorY,sz},{-sx,floorY,sz});
            face({-sx,-1,-sz},{sx,-1,-sz},{sx,1,-sz},{-sx,1,-sz});
            face({-sx,-1,-sz},{-sx,-1,sz},{-sx,1,sz},{-sx,1,-sz});
            floor_buffer.bind();floor_buffer.allocate(floor.data(),int(floor.size()*sizeof(float)));floor_buffer.release();
            grid_signature=gridSignature;
            setProperty("gridRevision",property("gridRevision").toULongLong()+1);
        }
        floor_buffer.bind();
        program->setUniformValue("floorPass", true);
        program->setUniformValue("floorSigns", QVector2D(wallX,wallZ));
        program->setUniformValue("gridExtent",QVector2D(scene.scaleX+.05f,scene.scaleZ+.05f));
        program->setAttributeBuffer(location, GL_FLOAT, 0, 3, 6 * sizeof(float));
        program->setAttributeBuffer(normal_location, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
        const auto grid_color = theme.gridLineColor();
        program->setUniformValue("gridPass", true);
        program->setUniformValue("model3d", true);
        program->setUniformValue("gridColor", QVector3D(grid_color.redF(), grid_color.greenF(), grid_color.blueF()));
        if (theme.isBackgroundEnabled()) {
            const auto color=theme.backgroundColor();
            program->setUniformValue("gridColor",QVector3D(color.redF(),color.greenF(),color.blueF()));
            glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1,1);
            glDrawArrays(GL_TRIANGLES,floorVertices+wallVertices,18); glDisable(GL_POLYGON_OFFSET_FILL);
            program->setUniformValue("gridColor",QVector3D(grid_color.redF(),grid_color.greenF(),grid_color.blueF()));
        }
        if(options["floor_grid"].toBool(true))glDrawArrays(GL_LINES,0,floorVertices);
        if(options["axis_grid"].toBool(true))glDrawArrays(GL_LINES,floorVertices,wallVertices);
        const auto axis_color = theme.labelTextColor();
        program->setUniformValue("gridColor", QVector3D(axis_color.redF(), axis_color.greenF(), axis_color.blueF()));
        // Wall lines already contain the native axis frame.
        floor_buffer.release();
        program->disableAttributeArray(location);
        program->disableAttributeArray(normal_location);
        buffer.release();
        program->release();
        core::perf::record(core::perf::Stage::SpectrogramWalls,wallsStart,core::perf::now()-wallsStart);
        const auto labelsStart=core::perf::now();
        // Glyph atlas and quad geometry are retained. Depth-tested billboards
        // avoid both overlay bleed-through and synchronous depth readbacks.
        QStringList texts;
        std::array<int,3> labelOffsets{};
        for(int axis=0;axis<3;++axis) {labelOffsets[axis]=int(texts.size());texts.append(tickLabels[axis]);}
        const int titleOffset=int(texts.size());
        texts << ui::text(axes.time_title_key.toUtf8().constData()) << ui::text(axes.column_title_key.toUtf8().constData()) << ui::text(axes.value_title_key.toUtf8().constData());
        const int atlasRows=(texts.size()+2)/3;
        const auto signature = texts.join('|') + font().toString() + theme.labelTextColor().name();
        if (signature != label_signature) {
            core::perf::Scope atlasTrace(core::perf::Stage::SpectrogramAtlas);
            const auto dpr = devicePixelRatioF();
            QImage atlas(qRound(768 * dpr), qRound(atlasRows * 32 * dpr), QImage::Format_RGBA8888);
            atlas.setDevicePixelRatio(dpr);
            atlas.fill(Qt::transparent);
            QPainter glyphs(&atlas);
            glyphs.setFont(font());
            glyphs.setPen(theme.labelTextColor());
            for (int index = 0; index < texts.size(); ++index)
                glyphs.drawText((index % 3) * 256, (index / 3) * 32 + QFontMetrics(font()).ascent() + 1, texts[index]);
            glyphs.end();
            label_texture = std::make_unique<QOpenGLTexture>(atlas.flipped(Qt::Vertical));
            label_texture->setMinificationFilter(QOpenGLTexture::Linear);
            label_texture->setMagnificationFilter(QOpenGLTexture::Linear);
            label_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            label_signature = signature;
        }
        QVector<QRectF> numericBounds{QRectF(width()-76,46,76,140)};
        if(data.empty() || (selection && selected.x()>=0))
            numericBounds.append(QRectF(12,8,std::max(1,width()-24),data.empty() ? 64 : 36));
        if (label_program->isLinked() && label_texture) {
            label_program->bind();
            label_texture->bind(0);
            label_buffer.bind();
            label_program->setUniformValue("mvp", projection * view);
            label_program->setUniformValue("viewport", QVector2D(width(), height()));
            label_program->setUniformValue("atlas", 0);
            const auto point = label_program->attributeLocation("point");
            label_program->enableAttributeArray(point);
            label_program->setAttributeBuffer(point, GL_FLOAT, 0, 2);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            auto label = [&](int index, QVector3D anchor, QVector2D offset, float angle=0) {
                const auto projected=projection*view*QVector4D(anchor,1);
                if (projected.w() <= 0)
                    return;
                QTransform rotation;rotation.rotate(-angle*180/3.141592653589793);
                auto bounds=rotation.mapRect(QRectF(0,0,QFontMetrics(font()).horizontalAdvance(texts[index]),QFontMetrics(font()).height()));
                bounds.translate((projected.x()/projected.w()+1)*width()/2+offset.x(),
                    (1-projected.y()/projected.w())*height()/2+offset.y()-QFontMetrics(font()).ascent()-1);
                const auto placed=spectrogramTickLabelRect(bounds,QRectF(rect()).adjusted(4,4,-4,-4),numericBounds);
                if(placed.isEmpty()) return;
                offset+=QVector2D(placed.left()-bounds.left(),placed.top()-bounds.top());
                numericBounds.append(placed.adjusted(-3,-3,3,3));
                label_program->setUniformValue("anchor", anchor);
                label_program->setUniformValue("angle", angle);
                label_program->setUniformValue("offset", offset - QVector2D(0, QFontMetrics(font()).ascent() + 1));
                label_program->setUniformValue(
                    "region", QVector4D((index % 3) / 3.f, 1 - float(index / 3 + 1) / atlasRows, 1 / 3.f, 1.f / atlasRows));
                glDrawArrays(GL_TRIANGLES, 0, 6);
            };
            // Reserve endpoints first, then interior ticks. Collision checks remain
            // the final guard for different axes and partially offscreen labels.
            for(int axis=0;axis<3;++axis) {
                std::vector<int> order{0,sections[axis]};
                for(int tick=1;tick<sections[axis];++tick)order.push_back(tick);
                for(int tick:order) {
                    const float fraction=float(tick)/sections[axis],p=-1+2*fraction;
                    const int index=labelOffsets[axis]+tick;
                    if(axis==0) {
                        if(!std::isfinite(timeIndices[tick]) || (tick && timeIndices[tick]==timeIndices[0]))continue;
                        const float timeX=scene.point(float(timeIndices[tick]),scene.zMin,scene.yMin).x();
                        label(index,{timeX,-1.f,wallZ*(scene.scaleZ+.25f)},{3,14},.523599f*wallX);
                    } else if(axis==1) {
                        if(scene.zMax>scene.zMin || tick==0)
                            label(index,{wallX*(scene.scaleX+.25f),-1.f,scene.zMax>scene.zMin ? -p*scene.scaleZ : 0.f},{3,14},-.523599f*wallZ);
                    } else label(index,{-wallX*(scene.scaleX+.1f),-1+2*fraction,wallZ*(scene.scaleZ+.1f)},{5,0});
                }
            }
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glDisable(GL_BLEND);
            label_program->disableAttributeArray(point);
            label_buffer.release();
            label_texture->release();
            label_program->release();
        }
        core::perf::record(core::perf::Stage::SpectrogramLabels,labelsStart,core::perf::now()-labelsStart);
        const auto overlayStart=core::perf::now();
        glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
        if(selection<2) setProperty("sliceSampleCount",0);
        QPainter painter(this);
        painter.setPen(theme.labelTextColor());
        // Titles follow their physical axes; screen-space offsets avoid tick labels.
        const auto centerProjection=projection*view*QVector4D(0,-1.f,0,1);
        const QPointF sceneCenter=centerProjection.w()>0 ?
            QPointF((centerProjection.x()/centerProjection.w()+1)*width()/2,(1-centerProjection.y()/centerProjection.w())*height()/2) :
            QPointF(width()/2.,height()/2.);
        const std::array<QVector3D,3> axisAnchors{{{0,-1.f,wallZ*(scene.scaleZ+.1f)},
            {wallX*(scene.scaleX+.1f),-1.f,0},
            {-wallX*(scene.scaleX+.1f),1.f,wallZ*(scene.scaleZ+.1f)}}};
        for(int axis=0;axis<3;++axis) {
            const auto projected=projection*view*QVector4D(axisAnchors[axis],1);if(projected.w()<=0) continue;
            const QPointF anchor((projected.x()/projected.w()+1)*width()/2,(1-projected.y()/projected.w())*height()/2);
            if(!QRectF(rect()).adjusted(-24,-24,24,24).contains(anchor)) continue;
            const QString title=texts[titleOffset+axis];const auto metrics=QFontMetrics(font());
            const auto bounds=spectrogramAxisTitleRect(anchor,anchor-sceneCenter,
                QSizeF(metrics.horizontalAdvance(title)+16,metrics.height()+8),QRectF(rect()).adjusted(4,4,-4,-4),numericBounds);
            if(bounds.isEmpty()) continue;
            painter.save();painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(theme.labelTextColor(),1));painter.drawLine(anchor,bounds.center());
            painter.setBrush(background);painter.drawRoundedRect(bounds,3,3);
            painter.drawText(bounds.adjusted(6,2,-6,-2),Qt::AlignCenter,
                metrics.elidedText(title,Qt::ElideRight,std::max(1,int(bounds.width()-12))));painter.restore();
            numericBounds.append(bounds.adjusted(-4,-4,4,4));
        }
        if (data.empty())
            painter.drawText(QRect(12, 12, std::max(1, width() - 24), 60), Qt::AlignLeft | Qt::TextWordWrap,
                             ui::text("Waiting for data. Check the source and connections in Data Flow."));
        const QString spacing=(axes.time_coordinates || axes.column_coordinates)
            ? ui::text("Indexed spacing; coordinates shown in labels") : QString{};
        const QString status=projection_status+(spacing.isEmpty()?QString{}:
            (projection_status.isEmpty()?QString{}:QStringLiteral(" · "))+spacing);
        if(!status.isEmpty())
            painter.drawText(QRect(12,std::max(12,height()-44),std::max(1,width()-24),32),Qt::AlignLeft | Qt::AlignBottom,
                QFontMetrics(painter.font()).elidedText(status,Qt::ElideRight,std::max(1,width()-24)));
        // This legend shows the exact original Qt gradient stops and data scale.
        QLinearGradient legend(0, 174, 0, 54);
        auto displayStops=stops;
        if(scene.reverseAmplitude) {for(auto &stop:displayStops) stop.first=1-stop.first;std::reverse(displayStops.begin(),displayStops.end());}
        legend.setStops(displayStops);
        const int legend_x = width() - 64;
        painter.fillRect(QRect(legend_x, 54, 14, 120), legend);
        painter.drawRect(QRect(legend_x, 54, 14, 120));
        std::vector<double> legendTicks;
        for (int tick=0; tick<=4; ++tick) legendTicks.push_back(scene.signalTick(tick/4.f));
        const auto legendLabels=axisTickLabels(legendTicks,axisLocale,precision);
        for (int tick=0; tick<=4; ++tick)
            painter.drawText(legend_x+20,178-tick*30,legendLabels[tick]);
        if (!data.empty() && selection && selected.x() >= 0 && selected.y() >= 0 && size_t(selected.y()) < data.size() &&
            size_t(selected.x()) < std::min(historySize(),data[size_t(selected.y())].size())) {
            setProperty("selectedAge",selected.x());setProperty("selectedBand",selected.y());
            setProperty("selectedValue",sample(selected.y(),selected.x()));
            const int precision=options["axis_precision"].toInt();
            const auto axisLabel=[precision,&axisLocale](double value){return axisReadoutLabel(value,axisLocale,precision);};
            const auto readout=spectrogramSelectionRows(axisLabel(axes.age(selected.x())),axisLabel(axes.column(selected.y())),
                axisLabel(sample(selected.y(),selected.x())),axes.time_coordinates,axes.column_title_key,axes.value_title_key,axes.age_axis,axes.time_title_key);
            painter.save();
            const auto readoutLayout=spectrogramReadoutLayout(readout,painter.font(),size());
            painter.setFont(readoutLayout.font);painter.setPen(theme.labelTextColor());
            for(int line=0;line<readoutLayout.lines.size();++line)
                painter.drawText(readoutLayout.baselines[line],readoutLayout.lines[line]);
            painter.restore();setProperty("selectionReadoutRows",readout);
            setProperty("selectionReadoutLines",readoutLayout.lines);
            setProperty("selectionReadoutRect",readoutLayout.bounds.toAlignedRect());
            const auto projected=projectedSample(selected.x(),selected.y(),scene,projection*view);
            setProperty("selectionMarkerVisible",false);
            if(projected.visible) {
                const QPointF center=projected.point;
                painter.save();painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(Qt::NoBrush);painter.setPen(QPen(Qt::black,5));painter.drawEllipse(center,8,8);
                painter.setPen(QPen(Qt::white,2));painter.drawEllipse(center,8,8);
                painter.setBrush(QColor(255,210,0));painter.setPen(QPen(Qt::black,1));painter.drawEllipse(center,3,3);
                painter.restore();setProperty("selectionMarkerVisible",rect().contains(center.toPoint()));
                setProperty("selectionMarkerPosition",center);
            }
            if (selection == 2 || selection == 3) {
                // A real cross-section, as in the native SelectionSlice mode.
                // Read the selected history/band directly; no GPU readback.
                const QRectF panel = sliceOverlay.rect(size());
                painter.save();
                painter.setClipRect(panel);
                setProperty("slicePanelRect",panel);
                const QColor panelColor=background;
                const QColor ink=spectrogramTextColor(panelColor);
                painter.fillRect(panel,panelColor);QPen framePen(ink);framePen.setCosmetic(true);painter.setPen(framePen);
                painter.drawRect(panel.adjusted(.5,.5,-.5,-.5));
                const auto plot = spectrogramSlicePlot(panel);
                const bool timeSlice=selection==2;
                const int begin=int(std::ceil(timeSlice ? scene.zMin : scene.xMin));
                const int end=std::min(int(timeSlice ? bandSize() : historySize()),
                                       timeSlice ? int(std::floor(scene.zMax))+1 : std::min(int(filled),int(std::floor(scene.xMax))+1));
                const auto sliceAxisPoint=[&](int index) {
                    // Use the axis plane, so signal amplitude cannot flip ordering.
                    return projection*view*QVector4D(scene.point(timeSlice?selected.x():index,
                        timeSlice?index:selected.y(),scene.signalTick(0)),1);
                };
                const bool reverseSlice=end>begin && spectrogramSliceReversed(sliceAxisPoint(begin),sliceAxisPoint(end-1));
                const int leftIndex=reverseSlice?end-1:begin,rightIndex=reverseSlice?begin:end-1;
                const auto coordinate=[&](int index){return timeSlice?axes.column(index):axes.age(index);};
                setProperty("sliceEndpointIndices",QPoint(leftIndex,rightIndex));
                setProperty("sliceEndpointCoordinates",QPointF(coordinate(leftIndex),coordinate(rightIndex)));
                if(end>begin)setProperty("sliceEndpointValues",QPointF(
                    timeSlice?sample(leftIndex,selected.x()):sample(selected.y(),leftIndex),
                    timeSlice?sample(rightIndex,selected.x()):sample(selected.y(),rightIndex)));
                painter.drawText(panel.adjusted(8,3,-8,-3), Qt::AlignTop,
                                  ui::text(timeSlice ? "Time Slice" : "Column Slice"));
                QLinearGradient fill(0,plot.bottom(),0,plot.top());
                fill.setStops(displayStops);
                QPolygonF section; section << plot.bottomLeft();
                for(int column=0;column<end-begin;++column) {
                    const int i=reverseSlice ? end-1-column : begin+column;
                    const float value=timeSlice ? sample(i,selected.x()) : sample(selected.y(),i);
                    section << QPointF(plot.left()+plot.width()*column/std::max(1,end-begin-1),
                                       plot.bottom()-plot.height()*std::clamp(scene.verticalFraction(value),0.f,1.f));
                }
                section << plot.bottomRight();
                painter.save();painter.setClipRect(plot);painter.setBrush(fill);
                painter.setPen(framePen);painter.drawPolygon(section);painter.restore();
                // The signal can lie exactly on the clip boundary at zero.
                // Draw its independent axes afterward, without the plot clip.
                painter.setBrush(Qt::NoBrush);painter.setPen(framePen);painter.drawRect(plot);
                painter.setPen(ink);
                painter.drawText(QPointF(panel.left()+8,plot.top()+5),axisLabel(scene.signalTick(1)));
                painter.drawText(QPointF(panel.left()+8,plot.bottom()),axisLabel(scene.signalTick(0)));
                painter.drawText(QRectF(plot.left(),plot.bottom()+2,plot.width()/2,18),Qt::AlignLeft | Qt::AlignTop,
                    axisLabel(coordinate(leftIndex)));
                painter.drawText(QRectF(plot.center().x(),plot.bottom()+2,plot.width()/2,18),Qt::AlignRight | Qt::AlignTop,
                    axisLabel(coordinate(rightIndex)));
                painter.drawText(QRectF(plot.left(),panel.bottom()-20,plot.width(),16),Qt::AlignCenter,
                    ui::text((timeSlice ? axes.column_title_key : axes.time_title_key).toUtf8().constData()));
                painter.save();painter.translate(panel.left()+14,plot.center().y());painter.rotate(-90);
                painter.drawText(QRectF(-plot.height()/2,-10,plot.height(),16),Qt::AlignCenter,ui::text(axes.value_title_key.toUtf8().constData()));painter.restore();
                painter.setBrush(Qt::NoBrush);painter.setPen(framePen);
                painter.drawRect(panel.adjusted(.5,.5,-.5,-.5));
                setProperty("sliceSampleCount",std::max(0,end-begin));
                painter.restore();
            }
        } else {setProperty("sliceSampleCount",0);setProperty("selectionMarkerVisible",false);setProperty("selectionReadoutRows",QStringList{});setProperty("selectionReadoutLines",QStringList{});setProperty("selectionReadoutRect",QRect{});}
        painter.end();
        core::perf::record(core::perf::Stage::SpectrogramOverlay,overlayStart,core::perf::now()-overlayStart);
        if(gpuActive) {gpu_query->end();gpu_pending=true;}
        last = clock.elapsed();
        ++frames;
        setProperty("renderedFrames", property("renderedFrames").toULongLong() + 1);
        if (last - start >= 1000) {
            rate = frames * 1000.0 / (last - start);
            frames = 0;
            start = last;
        }
    }
};

SpectrogramGraph::SpectrogramGraph(QWidget *parent) : QWidget(parent) {
    _surface_settings=SurfaceSettings::defaults();
    _camera_timer.setInterval(17);
    _camera_timer.setTimerType(Qt::PreciseTimer);
    connect(&_camera_timer, &QTimer::timeout, this, [this] { _move_camera(); });
}
SpectrogramGraph::~SpectrogramGraph() {
    _stop_camera();
    if (m_spectrogram_graph) {
        m_spectrogram_graph->prepare = {};
        delete m_spectrogram_graph;
        m_spectrogram_graph = nullptr;
    }
}
QVariantMap SpectrogramGraph::axis_layout_telemetry() const {
    QVariantMap result;
    if(!m_spectrogram_graph)return result;
    for(const auto* key:{"timeAxisSections","bandAxisSections","valueAxisSections",
                        "timeAxisTickValues","bandAxisTickValues","valueAxisTickValues",
                        "timeAxisTitle","bandAxisTitle","gridRevision","axisPolicyRevision",
                        "horizontalGridLines","horizontalGridLinesTotal","renderedFrames"})
        result.insert(QString::fromLatin1(key),m_spectrogram_graph->property(key));
    result.insert(QStringLiteral("paintCount"),m_spectrogram_graph->property("renderedFrames"));
    return result;
}
qreal SpectrogramGraph::display_fps() const {
    return _render_visible && m_spectrogram_graph && isVisible() ? m_spectrogram_graph->fps() : 0;
}
bool SpectrogramGraph::initialize(const QSize &, const QSize &) {
    if (_initialization_attempted)
        return m_spectrogram_graph;
    _initialization_attempted = true;
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal") {
        layout->addWidget(ui::make<QLabel>("3D visualization requires a native graphical session.", this));
        return false;
    }
    _splitter = new QSplitter(Qt::Horizontal, this);
    _splitter->setObjectName("spectrogramSplitter");
    _splitter->setChildrenCollapsible(false);
    _splitter->setHandleWidth(8);
    layout->addWidget(_splitter);
    m_spectrogram_graph = new SpectrogramRenderer(_splitter);
    m_spectrogram_graph->setRenderVisible(_render_visible);
    m_spectrogram_graph->setRenderFps(_render_fps);
    m_spectrogram_graph->history_capacity=_history_capacity;
    m_spectrogram_graph->configured_bands=_history_bands;
    m_spectrogram_graph->axes=_axis_metadata;
    m_spectrogram_graph->projection_status=_projection_status;
    m_spectrogram_graph->options=surface_settings();
    m_spectrogram_graph->shadow_error=[this](const QString &message){emit surface_settings_error(message);};
    connect(&ui::Language::instance(),&ui::Language::changed,m_spectrogram_graph,[this]{m_spectrogram_graph->requestUpdate();});
    m_container = m_spectrogram_graph;
    m_container->installEventFilter(this);
    m_container->setMouseTracking(true);
    desktop_input::attach_touchpad(m_container,[this](QPoint point) {
        return _render_visible && !(m_spectrogram_graph->selection>=2 &&
            m_spectrogram_graph->property("sliceSampleCount").toInt()>0 &&
            m_spectrogram_graph->sliceOverlay.rect(m_container->size()).contains(point));
    });
    _splitter->addWidget(m_container);
    auto *scroll = new QScrollArea(_splitter);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(200);
    auto *panel = new QWidget(scroll);
    panel->setMinimumWidth(0);panel->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    scroll->setWidget(panel);
    auto *controls = new QVBoxLayout(panel);
    controls->setAlignment(Qt::AlignTop);
    _splitter->addWidget(scroll);
    _splitter->setStretchFactor(0, 1);
    auto group=[&](const char *title) {
        auto *box=ui::make<QGroupBox>(title,panel);auto *layout=new QVBoxLayout(box);
        box->setMinimumWidth(0);box->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
        controls->addWidget(box);return layout;
    };
    auto *signalControls=group("Playback");
    auto *pause = ui::make<QCheckBox>("Pause", panel);
    pause->setObjectName("spectrogramPause");
    signalControls->addWidget(pause);
    connect(pause, &QCheckBox::toggled, this, [this](bool value) {
        set_paused(value);
        emit presentation_changed(presentation_state());
    });
    auto *selectionControls=group("Selection");
    auto *selection = new QComboBox(panel);
    selection->setObjectName("spectrogramSelection");
    ui::items(selection, {"No selection", "Item", "Time Slice", "Frequency Slice"});
    selection->setCurrentIndex(1);
    selection->hide();
    const char *modeNames[]{"No selection","Item","Time Slice","Frequency Slice"};
    for(int index=0;index<4;++index) {
        auto *button=ui::make<QRadioButton>(modeNames[index],panel);
        selectionControls->addWidget(button);button->setChecked(index==selection->currentIndex());
        connect(button,&QRadioButton::toggled,this,[selection,index](bool checked){if(checked)selection->setCurrentIndex(index);});
        connect(selection,&QComboBox::currentIndexChanged,button,[button,index](int value){button->setChecked(value==index);});
    }
    connect(selection, &QComboBox::currentIndexChanged, this, [this](int value) {
        m_spectrogram_graph->endPointDrag();
        m_spectrogram_graph->selection = value;
        m_spectrogram_graph->requestUpdate();
        emit presentation_changed(presentation_state());
    });
    connect(&ui::Theme::instance(), &ui::Theme::changed, m_spectrogram_graph,
            [this] { m_spectrogram_graph->requestUpdate(); });
    auto* aspect=new ui::SceneAspectControl(panel);group("Scene geometry")->addWidget(aspect);
    aspect->changed=[this,aspect]{
        set_surface_settings(aspect->apply(surface_settings()));
        emit surface_settings_changed(surface_settings());
    };
    connect(this,&SpectrogramGraph::surface_settings_updated,aspect,[aspect](const QJsonObject& value){aspect->set_options(value);});
    auto *axisXControls=group("Axis range X");
    auto *axisYControls=group("Axis range Y");
    auto *axisZControls=group("Axis range Z");
    auto *xAxis=new AxisControlWidget("Axis range X",panel);
    auto *yAxis=new AxisControlWidget("Axis range Y",panel);
    auto *zAxis=new AxisControlWidget("Axis range Z",panel);
    xAxis->setObjectName("spectrogramAxisX");yAxis->setObjectName("spectrogramAxisY");zAxis->setObjectName("spectrogramAxisZ");
    xAxis->set_bounds(0,100);yAxis->set_bounds(-1e6,1e6);zAxis->set_bounds(0,100);
    axisXControls->addWidget(xAxis);axisYControls->addWidget(yAxis);axisZControls->addWidget(zAxis);
    auto* rangeInterval=new ui::AutoRangeIntervalControl(panel);axisYControls->addWidget(rangeInterval);
    rangeInterval->box->setObjectName("spectrogramAutoRangeIntervalSeconds");
    connect(rangeInterval->box,&QDoubleSpinBox::valueChanged,this,[this,rangeInterval]{
        auto options=surface_settings();options["auto_range_interval_seconds"]=rangeInterval->seconds();
        set_surface_settings(options);emit surface_settings_changed(surface_settings());
    });
    connect(this,&SpectrogramGraph::surface_settings_updated,rangeInterval,[rangeInterval](const QJsonObject& options){
        rangeInterval->set_seconds(options["auto_range_interval_seconds"].toDouble(10));
        rangeInterval->setEnabled(options["y_range_mode"]!="manual");
    });
    auto applySurface=[this](const char *key,const QJsonValue &value) {
        auto options=surface_settings();options[key]=value;set_surface_settings(options);
        emit surface_settings_changed(surface_settings());
    };
    auto applyHorizontalRange=[this](int axis,double low,double high,bool automatic) {
        const int minimum=automatic ? 0 : std::clamp(qRound(low),0,99);
        const int maximum=automatic ? 100 : std::clamp(qRound(high),minimum+1,100);
        m_spectrogram_graph->ranges[axis*2]=minimum;m_spectrogram_graph->ranges[axis*2+1]=maximum;
        m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
        emit presentation_changed(presentation_state());
    };
    connect(xAxis,&AxisControlWidget::range_changed,this,[applyHorizontalRange](double low,double high,bool automatic){applyHorizontalRange(0,low,high,automatic);});
    connect(zAxis,&AxisControlWidget::range_changed,this,[applyHorizontalRange](double low,double high,bool automatic){applyHorizontalRange(1,low,high,automatic);});
    connect(yAxis,&AxisControlWidget::range_changed,this,[this](double low,double high,bool automatic){
        auto options=surface_settings();options["y_range_mode"]=automatic ? "auto" : "manual";
        options["y_min"]=low;options["y_max"]=high;set_surface_settings(options);emit surface_settings_changed(surface_settings());
    });
    connect(xAxis,&AxisControlWidget::reverse_changed,this,[applySurface](bool reverse){applySurface("reverse_time",reverse);});
    connect(yAxis,&AxisControlWidget::reverse_changed,this,[applySurface](bool reverse){applySurface("reverse_amplitude",reverse);});
    connect(zAxis,&AxisControlWidget::reverse_changed,this,[applySurface](bool reverse){applySurface("reverse_bands",reverse);});
    auto syncAxes=[this,xAxis,yAxis,zAxis](const QJsonObject &options) {
        const auto *ranges=m_spectrogram_graph->ranges;
        xAxis->set_state(ranges[0],ranges[1],ranges[0]==0 && ranges[1]==100 && xAxis->auto_box()->isChecked(),options["reverse_time"].toBool());
        const auto extent=y_range();
        yAxis->set_state(extent.first,extent.second,options["y_range_mode"]!="manual",options["reverse_amplitude"].toBool());
        zAxis->set_state(ranges[2],ranges[3],ranges[2]==0 && ranges[3]==100 && zAxis->auto_box()->isChecked(),options["reverse_bands"].toBool());
    };
    connect(this,&SpectrogramGraph::surface_settings_updated,panel,syncAxes);
    connect(this,&SpectrogramGraph::y_range_changed,panel,[this,syncAxes](float,float){syncAxes(surface_settings());});
    auto refreshExtent=[this,xAxis,zAxis] {
        xAxis->set_hint(ui::text("Range (%)")+"\n"+ui::text(_axis_metadata.time_title_key.toUtf8().constData())+"\n"+ui::text("History: %1 frames").arg(QString("%1 / %2")
            .arg(_spectrogram.empty() ? 0 : _spectrogram.front().size()).arg(m_spectrogram_graph->historySize())));
        zAxis->set_hint(ui::text("Range (%)")+"\n"+ui::text(_axis_metadata.column_title_key.toUtf8().constData())+"\n"+ui::text("Columns: %1").arg(m_spectrogram_graph->bandSize()));
    };
    auto *extentTimer=new QTimer(panel);extentTimer->setInterval(500);
    connect(extentTimer,&QTimer::timeout,panel,[this,refreshExtent]{if(isVisible())refreshExtent();});refreshExtent();extentTimer->start();
    auto *colorControls=group("Color scale");
    auto *preset=new QComboBox(panel);preset->setObjectName("spectrogramColorPreset");
    preset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);preset->setMinimumContentsLength(8);preset->setMinimumWidth(0);
    const QStringList presetCodes{"green_red","black_yellow","viridis","ice_fire","rainbow"};
    ui::items(preset,{"Green / Red","Black / Yellow","Viridis","Ice / Fire","Rainbow","Custom"});colorControls->addWidget(preset);
    connect(preset,&QComboBox::activated,this,[this,presetCodes](int index){
        if(index==presetCodes.size()) {emit edit_color_scale_requested();emit surface_settings_updated(surface_settings());return;}
        if(index<0 || index>=presetCodes.size()) return;
        auto options=surface_settings();options["gradient"]=presetCodes[index];options["gradient_stops"]=QJsonArray{};
        set_surface_settings(options);emit surface_settings_changed(surface_settings());
    });
    connect(this,&SpectrogramGraph::surface_settings_updated,panel,[preset,presetCodes](const QJsonObject &options){
        const QSignalBlocker block(preset);
        preset->setCurrentIndex(!options["gradient_stops"].toArray().isEmpty() ? presetCodes.size() :
            std::max(0,int(presetCodes.indexOf(options["gradient"].toString()))));
    });
    auto *cameraControls=group("Camera");
    auto *reset = ui::make<QPushButton>("Reset (Alt+R)", panel);
    reset->setObjectName("resetCamera");
    cameraControls->addWidget(reset);
    connect(reset, &QPushButton::clicked, this, &SpectrogramGraph::reset_camera);
    reset->setToolTip(ui::text("Reset camera (Alt+R)"));
    const auto baselineOrientation=QQuaternion::fromEulerAngles(-45,45,0);
    const float framing=std::min(1.f,.625f*m_container->width()/std::max(1,m_container->height()));
    const auto baselinePosition=baselineOrientation.rotatedVector({0,0,6.f/(1.55f*framing)});
    _default_camera_state = {{"version",2},
        {"position",QJsonArray{baselinePosition.x(),baselinePosition.y(),baselinePosition.z()}},
        {"quaternion",QJsonArray{baselineOrientation.scalar(),baselineOrientation.x(),baselineOrientation.y(),baselineOrientation.z()}},
        };
    set_camera_state(_camera_state.isEmpty() ? _default_camera_state : _camera_state);
    set_presentation_state(_presentation_state);
    set_surface_settings(_surface_settings);
    m_spectrogram_graph->setData(_spectrogram);
    set_side_panel_ratio(_side_panel_ratio);
    connect(_splitter, &QSplitter::splitterMoved, this, [this] {
        const auto sizes = _splitter->sizes();
        if (sizes[0] + sizes[1]) {
            _side_panel_ratio = float(sizes[1]) / (sizes[0] + sizes[1]);
            emit side_panel_resized(_side_panel_ratio);
        }
    });
    return true;
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
        return;
    }
    _pause=false;
    auto pending=std::move(_pending_projection);
    _pending_projection={};
    set_history_dimensions(pending.capacity,pending.bands);
    set_axis_metadata(pending.axes);
    set_projection_status(pending.status);
    update_snapshot(std::move(pending.data));
}
void SpectrogramGraph::set_history_dimensions(size_t capacity,size_t bands) {
    if(!capacity || !bands || capacity>16384 || bands>4096 || capacity*bands>1048576) return;
    if(_pause){_pending_projection.capacity=capacity;_pending_projection.bands=bands;return;}
    if(capacity==_history_capacity && bands==_history_bands) return;
    _history_capacity=capacity;_history_bands=bands;
    if(m_spectrogram_graph) {
        m_spectrogram_graph->history_capacity=capacity;m_spectrogram_graph->configured_bands=bands;
        if(m_spectrogram_graph->selected.x()>=int(capacity) || m_spectrogram_graph->selected.y()>=int(bands))
            m_spectrogram_graph->selected={-1,-1};
        m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
    }
}
bool SpectrogramGraph::set_axis_metadata(const SpectrogramAxisMetadata& value) {
    if(value.ages.size()>16384 || value.row_indices.size()>16384 || value.columns.size()>4096 || value.time_title_key.size()>128 || value.column_title_key.size()>128 || value.value_title_key.size()>128 || value.time_index_title_key.size()>128 || value.column_index_title_key.size()>128)return false;
    const auto valid=[](const std::vector<double>& coordinates,bool eitherDirection=false) {
        const bool descending=eitherDirection && coordinates.size()>1 && coordinates[1]<coordinates[0];
        for(std::size_t i=0;i<coordinates.size();++i)
            if(!std::isfinite(coordinates[i]) || (i && (descending?coordinates[i]>=coordinates[i-1]:coordinates[i]<=coordinates[i-1])))return false;
        return true;
    };
    if(!valid(value.ages,!value.age_axis) || !valid(value.row_indices,true) || !valid(value.columns) || (value.age_axis && value.time_coordinates && !value.ages.empty() && value.ages[0]!=0))return false;
    if(_pause){_pending_projection.axes=value;return true;}
    const bool sourceChanged=_source_axis_metadata!=value;
    _source_axis_metadata=value;
    const auto displayed=value.displayed(_frequency_axis_mode,_time_axis_mode);
    if(_axis_metadata==displayed){if(sourceChanged)emit axis_metadata_changed();return true;}
    _axis_metadata=displayed;
    if(m_spectrogram_graph){m_spectrogram_graph->axes=displayed;m_spectrogram_graph->requestUpdate();}
    if(sourceChanged)emit axis_metadata_changed();
    return true;
}
void SpectrogramGraph::set_axis_display_modes(const QString& frequencyMode,const QString& timeMode) {
    _frequency_axis_mode=frequencyMode=="positions"?"positions":"coordinates";
    _time_axis_mode=timeMode=="positions"?"positions":"coordinates";
    _axis_metadata=_source_axis_metadata.displayed(_frequency_axis_mode,_time_axis_mode);
    if(m_spectrogram_graph){m_spectrogram_graph->axes=_axis_metadata;m_spectrogram_graph->requestUpdate();}
}
void SpectrogramGraph::set_projection_status(const QString& value) {
    if(_pause){_pending_projection.status=value;return;}
    if(_projection_status==value)return;
    _projection_status=value;
    if(m_spectrogram_graph){m_spectrogram_graph->projection_status=value;m_spectrogram_graph->requestUpdate();}
}
void SpectrogramGraph::set_render_fps(int fps) {
    _render_fps=std::clamp(fps,1,240);
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
void SpectrogramGraph::set_surface_settings(const QJsonObject &effective) {
    const auto previousY=y_range();
    const bool wasManual=surface_settings()["y_range_mode"]=="manual";
    _surface_settings=SurfaceSettings::normalized(effective);
    if(wasManual && _surface_settings["y_range_mode"]!="manual")_amplitude_range.reset();
    _observe_amplitude_range();
    if(m_spectrogram_graph) {
        const bool changed=m_spectrogram_graph->options!=_surface_settings;
        const bool remap=m_spectrogram_graph->options["reverse_time"]!=_surface_settings["reverse_time"] ||
            m_spectrogram_graph->options["reverse_bands"]!=_surface_settings["reverse_bands"] ||
            m_spectrogram_graph->options["reverse_amplitude"]!=_surface_settings["reverse_amplitude"] ||
            m_spectrogram_graph->options["y_range_mode"]!=_surface_settings["y_range_mode"] ||
            m_spectrogram_graph->options["y_min"]!=_surface_settings["y_min"] ||
            m_spectrogram_graph->options["y_max"]!=_surface_settings["y_max"] ||
            m_spectrogram_graph->options["scene_aspect_mode"]!=_surface_settings["scene_aspect_mode"] ||
            m_spectrogram_graph->options["scene_time_aspect"]!=_surface_settings["scene_time_aspect"] ||
            m_spectrogram_graph->options["scene_band_aspect"]!=_surface_settings["scene_band_aspect"];
        m_spectrogram_graph->options=_surface_settings;
        if(remap) m_spectrogram_graph->upload=true;
        m_spectrogram_graph->gradient=_surface_settings["gradient"]=="black_yellow" ? 0 : 1;
        if(changed) m_spectrogram_graph->requestUpdate();
    }
    const auto currentY=y_range();if(previousY!=currentY) emit y_range_changed(currentY.first,currentY.second);
    emit surface_settings_updated(_surface_settings);
}
void SpectrogramGraph::update_frame_history(const std::vector<std::vector<float>> &history) {
    update_snapshot(NumericMatrixSnapshot::take(history).transposed());
}
void SpectrogramGraph::update(const std::vector<std::vector<float>> &value) {
    update_snapshot(NumericMatrixSnapshot::take(value));
}
void SpectrogramGraph::update_snapshot(NumericMatrixSnapshot snapshot) {
    core::perf::Scope trace(core::perf::Stage::SpectrogramData);
    const size_t length=snapshot.empty()?0:snapshot.front().size();
    if(snapshot.size()>4096 || length>16384)snapshot.clear();
    if(_pause){_pending_projection.data=std::move(snapshot);return;}
    const auto previousY=y_range();
    const bool changed=_spectrogram!=snapshot;
    _spectrogram=std::move(snapshot);
    const bool rangeChanged=_observe_amplitude_range();
    if(!changed && !rangeChanged){
        if(m_spectrogram_graph)m_spectrogram_graph->setData(_spectrogram,false);
        return;
    }
    ++_history_projection_revision;
    if(!_history_capacity && !_spectrogram.empty() && m_spectrogram_graph) {
        m_spectrogram_graph->history_capacity=std::max(m_spectrogram_graph->history_capacity,length);
        m_spectrogram_graph->configured_bands=_spectrogram.size();
    }
    if(m_spectrogram_graph)m_spectrogram_graph->setData(_spectrogram);
    const auto currentY=y_range();if(previousY!=currentY)emit y_range_changed(currentY.first,currentY.second);
}
bool SpectrogramGraph::_observe_amplitude_range() {
    if(_pause || surface_settings()["y_range_mode"]=="manual" || _spectrogram.empty())return false;
    double low=0,high=0;
    for(size_t row=0;row<_spectrogram.size();++row)for(size_t column=0;column<_spectrogram[row].size();++column){const float value=_spectrogram[row][column];low=std::min(low,double(value));high=std::max(high,double(value));}
    const bool changed=_amplitude_range.observe(low,high,amplitude_clock(),surface_settings()["auto_range_interval_seconds"].toDouble(10));
    if(m_spectrogram_graph) {
        const auto bounds=_amplitude_range.range();
        m_spectrogram_graph->automatic_range={float(bounds.first),float(bounds.second)};
        if(changed){m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();}
    }
    return changed;
}
QJsonObject SpectrogramGraph::camera_state() const {
    auto state=_camera_state;
    state["version"]=2;
    state["position"]=QJsonArray{_position.x(),_position.y(),_position.z()};
    state["quaternion"]=QJsonArray{_orientation.scalar(),_orientation.x(),_orientation.y(),_orientation.z()};
    return state;
}
void SpectrogramGraph::set_camera_state(const QJsonObject &state) {
    if (state.isEmpty())
        return;
    auto p = state.value("position").toArray(), q = state.value("quaternion").toArray();
    auto finite = [](const QJsonArray &array, int size) {
        if (array.size() != size)
            return false;
        for (const auto &v : array)
            if (!v.isDouble() || (!std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 1000000))
                return false;
        return true;
    };
    if (finite(p, 3) && finite(q, 4)) {
        _position = {float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())};
        QQuaternion candidate(q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble());
        if (candidate.lengthSquared() > 1e-8)
            _orientation = CameraGeometry::uprightLook(candidate.normalized());
    } else if (state.contains("zoom")) {
        const float yaw = state.value("horizontal").toDouble(), pitch = state.value("vertical").toDouble(30),
                    zoom = state.value("zoom").toDouble(100);
        if (std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(zoom)) {
            _orientation = CameraGeometry::uprightLook(QQuaternion::fromEulerAngles(-pitch, -yaw, 0));
            _position = _orientation.rotatedVector({0, 0, std::clamp(300 / std::max(1.f, zoom), .01f, 10.f)});
            auto target = state.value("target").toArray();
            if (finite(target, 3))
                _position += QVector3D(target[0].toDouble(), target[1].toDouble(), target[2].toDouble());
        }
    }
    if (_position.length() > 10)
        _position = _position.normalized() * 10;
    _movement_speed=CameraControls::movementSpeed(_camera_controls);
    if (m_spectrogram_graph) {
        m_spectrogram_graph->position = _position;
        m_spectrogram_graph->orientation = _orientation;
        m_spectrogram_graph->requestUpdate();
    }
    _camera_state = state;
    _camera_state = camera_state();
}
void SpectrogramGraph::_camera_changed() {
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
void SpectrogramGraph::_refresh_camera_keys(Qt::KeyboardModifiers modifiers) {
    _camera_keys.clear();
    for(const auto& held:_held_camera_keys) {
        const QKeyEvent event(QEvent::KeyPress,held.second,modifiers,quint32(held.first),0,0);
        for(auto it=_camera_controls.begin();it!=_camera_controls.end();++it)
            for(const auto& binding:it.value().toArray())
                if(it.key()!="reset" && CameraControls::matches(binding.toObject(),event,it.key()=="fast"))_camera_keys.insert(it.key());
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
    if (_splitter)
        _splitter->setSizes({int(1000 * (1 - _side_panel_ratio)), int(1000 * _side_panel_ratio)});
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
    if (m_spectrogram_graph)
        m_spectrogram_graph->requestUpdate();
}
void SpectrogramGraph::hideEvent(QHideEvent *event) {
    _stop_camera();
    QWidget::hideEvent(event);
}
bool SpectrogramGraph::eventFilter(QObject *watched, QEvent *event) {
    if (!m_spectrogram_graph)
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::PaletteChange)
        m_spectrogram_graph->requestUpdate();
    if(event->type()==QEvent::Resize || event->type()==QEvent::Hide) {
        m_spectrogram_graph->sliceOverlay.end();m_spectrogram_graph->endPointDrag();
    }
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate)
        { _stop_camera();m_spectrogram_graph->sliceOverlay.end();m_spectrogram_graph->endPointDrag(); }
    if(!_render_visible) return QWidget::eventFilter(watched,event);
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouse=static_cast<QMouseEvent *>(event);
        _mouse_position = mouse->position().toPoint();
        m_container->setFocus();
        if(mouse->button()==Qt::LeftButton) {
            m_spectrogram_graph->endPointDrag();
            if(!(mouse->buttons() & (Qt::RightButton|Qt::MiddleButton)) && !m_spectrogram_graph->beginSliceDrag(mouse->position()))
                m_spectrogram_graph->beginPointDrag(mouse->position());
        }
        if(mouse->button()==Qt::RightButton || mouse->button()==Qt::MiddleButton) {
            m_spectrogram_graph->endPointDrag();m_spectrogram_graph->sliceOverlay.end();
        }
        if(mouse->button()==Qt::LeftButton || mouse->button()==Qt::RightButton || mouse->button()==Qt::MiddleButton) return true;
    }
    if (event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if(!(mouse->buttons() & Qt::LeftButton))m_spectrogram_graph->endPointDrag();
        if(m_spectrogram_graph->sliceOverlay.active && (mouse->buttons() & Qt::LeftButton)) {
            m_spectrogram_graph->moveSlice(mouse->position());return true;
        }
        if(m_spectrogram_graph->point_drag && (mouse->buttons() & Qt::LeftButton) && !(mouse->buttons() & (Qt::RightButton|Qt::MiddleButton))) {
            m_spectrogram_graph->pick(mouse->position(),true);return true;
        }
        if(mouse->buttons()==Qt::NoButton) {
            const bool slice=m_spectrogram_graph->selection>=2 && m_spectrogram_graph->property("sliceSampleCount").toInt()>0;
            m_container->setCursor(slice ? m_spectrogram_graph->sliceOverlay.cursor(m_container->size(),mouse->position()) : Qt::ArrowCursor);
        }
        if (mouse->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
            auto delta = mouse->position().toPoint() - _mouse_position;
            _mouse_position = mouse->position().toPoint();
            if(mouse->buttons() & Qt::MiddleButton)
                _position+=_orientation.rotatedVector(QVector3D(-delta.x(),delta.y(),0))*.005f;
            else _orientation = CameraGeometry::uprightLook(_orientation,-delta.x()*.3f,-delta.y()*.3f);
            _camera_changed();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton) {
        const bool captured=m_spectrogram_graph->point_drag || m_spectrogram_graph->sliceOverlay.active;
        if(m_spectrogram_graph->sliceOverlay.active) {
            m_spectrogram_graph->sliceOverlay.end();emit presentation_changed(presentation_state());
        }
        m_spectrogram_graph->endPointDrag();if(captured)return true;
    }
    // Orbit around the scene center; rotate position and orientation together.
    auto orbit=[this](double pixels) {
        const auto rotation=QQuaternion::fromAxisAndAngle({0,1,0},float(-pixels*.3));
        _position=rotation.rotatedVector(_position);
        _orientation=(rotation*_orientation).normalized();
    };
    auto zoom=[this](double amount) {
        _position+=_orientation.rotatedVector({0,0,float(-amount)});
        if(_position.length()>10) _position=_position.normalized()*10;
    };
    if(event->type()==QEvent::NativeGesture) {
        const auto *gesture=static_cast<QNativeGestureEvent *>(event);
        if(gesture->gestureType()==Qt::PanNativeGesture) {
            orbit(gesture->delta().x());zoom(gesture->delta().y()/150.0);
        } else if(gesture->gestureType()==Qt::ZoomNativeGesture) {
            const double ratio=std::clamp(1+gesture->value(),.1,10.0);
            _position/=float(ratio);
            if(_position.length()>10) _position=_position.normalized()*10;
        } else return QWidget::eventFilter(watched,event);
        _camera_changed();return true;
    }
    if (event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        if(!wheel->pixelDelta().isNull()) {
            orbit(wheel->pixelDelta().x());zoom(wheel->pixelDelta().y()/150.0);
        } else {
            orbit(wheel->angleDelta().x()/4.0);zoom(wheel->angleDelta().y()/600.0);
        }
        _camera_changed();return true;
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            _stop_camera();
            return true;
        }
        // Physical R also works with the Russian layout, before the Alt guard.
        const bool resetChord=key->modifiers()==Qt::AltModifier &&
            (CameraControls::scan(*key)==19 || CameraControls::logical(key->key())==Qt::Key_R);
        if(resetChord) {
            if(event->type()==QEvent::KeyPress && !key->isAutoRepeat()) reset_camera();
            return true;
        }
        const int scan = CameraControls::scan(*key),
                  identity = scan ? scan : CameraControls::logical(key->key()) + 0x10000;
        auto modifiers=key->modifiers();
        if(key->key()==Qt::Key_Control) {
            if(event->type()==QEvent::KeyPress)modifiers|=Qt::ControlModifier;
            else modifiers&=~Qt::ControlModifier;
            _refresh_camera_keys(modifiers);
            return false;
        }
        if (event->type() == QEvent::KeyRelease && !key->isAutoRepeat() && _held_camera_keys.remove(identity)) {
            _refresh_camera_keys(modifiers);
            return true;
        }
        if (key->modifiers() & (Qt::AltModifier | Qt::MetaModifier))return false;
        for (auto it = _camera_controls.begin(); it != _camera_controls.end(); ++it)
            for (const auto &binding : it.value().toArray())
                if (CameraControls::matches(binding.toObject(), *key,it.key()=="fast")) {
                    if(key->isAutoRepeat())return true;
                    if (event->type() == QEvent::KeyPress) {
                        if (it.key() == "reset")
                            reset_camera();
                        else {
                            _held_camera_keys[identity] = {scan,key->key()};
                            _refresh_camera_keys(modifiers);
                        }
                    }
                    return true;
                }
    }
    return QWidget::eventFilter(watched, event);
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
void SpectrogramGraph::set_presentation_state(const QJsonObject &state) {
    _presentation_state = state;
    set_paused(state.value("pause").toBool(false));
    if (!m_spectrogram_graph)
        return;
    m_spectrogram_graph->sliceOverlay.end();
    m_spectrogram_graph->sliceOverlay.preferredSize={};
    const auto sliceSize=state.value("slice_size").toArray();
    if(sliceSize.size()==2) {
        const double w=sliceSize[0].toDouble(),h=sliceSize[1].toDouble();
        if(std::isfinite(w) && std::isfinite(h) && w>0 && h>0)
            m_spectrogram_graph->sliceOverlay.preferredSize={std::clamp(w,180.0,16384.0),std::clamp(h,120.0,16384.0)};
    }
    const auto slicePosition=state.value("slice_position").toArray();
    if(slicePosition.size()==2) {
        const double x=slicePosition[0].toDouble(1),y=slicePosition[1].toDouble(1);
        if(std::isfinite(x) && std::isfinite(y))
            m_spectrogram_graph->sliceOverlay.position={std::clamp(x,0.0,1.0),std::clamp(y,0.0,1.0)};
    }
    const auto ranges = state.value("ranges").toArray();
    for (int axis = 0; axis < 2; ++axis) {
        int low = ranges.size() == 4 ? std::clamp(ranges[axis * 2].toInt(), 0, 99) : 0;
        int high = ranges.size() == 4 ? std::clamp(ranges[axis * 2 + 1].toInt(), 1, 100) : 100;
        if (low >= high)
            high = low + 1;
        m_spectrogram_graph->ranges[axis * 2] = low;
        m_spectrogram_graph->ranges[axis * 2 + 1] = high;
    }
    m_spectrogram_graph->selection = std::clamp(state.value("selection").toInt(1), 0, 3);
    set_paused(state.value("pause").toBool(false));
    for (auto name : {"spectrogramSelection"})
        if (auto *combo = findChild<QComboBox *>(name)) {
            QSignalBlocker block(combo);
            combo->setCurrentIndex(QString::fromLatin1(name) == "spectrogramGradient" ? m_spectrogram_graph->gradient
                                                                                      : m_spectrogram_graph->selection);
        }
    for (auto name : {"spectrogramPause"})
        if (auto *check = findChild<QCheckBox *>(name)) {
            QSignalBlocker block(check);
            check->setChecked(QString::fromLatin1(name) == "spectrogramPause" ? _pause : m_spectrogram_graph->model3d);
        }
    const auto radios=findChildren<QRadioButton *>();
    for(int index=0;index<radios.size();++index) {QSignalBlocker block(radios[index]);radios[index]->setChecked(index==m_spectrogram_graph->selection);}
    m_spectrogram_graph->upload = true;
    m_spectrogram_graph->requestUpdate();
    emit surface_settings_updated(surface_settings());
}
