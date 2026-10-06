#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include <QtDataVisualization/Q3DTheme>
#include "visualization/ui_text.h"
#include "visualization/surface_settings.h"
#include "visualization/axis_control_widget.h"
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
    std::vector<std::vector<float>> data;
    bool upload = true, dirty = false;
    bool render_visible=true;
    int gradient = 1, selection = 1;
    bool model3d = true;
    size_t history_capacity=0, configured_bands=0;
    QJsonObject options=SurfaceSettings::defaults();
    QPoint selected{-1, -1};
    QPointF slicePosition{1,1}, sliceDragOffset;
    bool sliceDragging=false;
    bool beginSliceDrag(QPointF point) {
        if(selection<2 || property("sliceSampleCount").toInt()<=0) return false;
        const auto panel=spectrogramSlicePanel(size(),slicePosition);
        if(!panel.contains(point)) return false;
        sliceDragging=true;sliceDragOffset=point-panel.topLeft();return true;
    }
    void moveSlice(QPointF point) {
        const auto origin=spectrogramSlicePanel(size(),{0,0}).topLeft();
        const auto span=spectrogramSlicePanel(size(),{1,1}).topLeft()-origin;
        const auto target=point-sliceDragOffset-origin;
        slicePosition={span.x()>0 ? std::clamp(target.x()/span.x(),0.0,1.0) : 0,
                       span.y()>0 ? std::clamp(target.y()/span.y(),0.0,1.0) : 0};
        requestUpdate();
    }
    int ranges[4]{0, 100, 0, 100};
    SpectrogramSceneGeometry geometry() const {
        const bool manual=options["y_range_mode"]=="manual";
        return {historySize(),bandSize(),ranges,options["reverse_time"].toBool(),options["reverse_bands"].toBool(),
            manual ? float(options["y_min"].toDouble(0)) : 0.f,manual ? float(options["y_max"].toDouble(1)) : 2.f,
            manual,options["reverse_amplitude"].toBool()};
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
        dirty|=frame_pending;frame_pending=false;
        rate=0;frames=0;last_frame=0;
        if(clock.isValid()) start=last=clock.elapsed();
        if(visible) {request_time=core::perf::now();requestUpdate();}
    }
    void setData(const std::vector<std::vector<float>> &value) {
        core::perf::Scope handoff(core::perf::Stage::SpectrogramHandoff);
        data = value;
        upload = true;
        requestUpdate();
    }
    void pick(const QPoint &point) {
        if(selection>=2 && property("sliceSampleCount").toInt()>0 && property("slicePanelRect").toRectF().contains(point)) return;
        if (!bandSize() || !historySize())
            return;
        QMatrix4x4 projection;
        projection.perspective(45, float(width()) / std::max(1, height()), .005f, 100);
        QMatrix4x4 view;
        view.rotate(orientation.conjugated());
        view.translate(-position);
        double best = 900;
        QPoint pick{-1, -1};
        const auto bands=bandSize(),history=historySize();
        for (size_t b = 0; b < bands; b += std::max(size_t(1), bands / 128))
            for (size_t t = 0; t < history; t += std::max(size_t(1), history / 128)) {
                const auto scene = geometry();
                if (t < scene.xMin || t > scene.xMax || b < scene.zMin || b > scene.zMax) continue;
                auto p = scene.point(t, b, sample(b,t));
                if (!model3d) p.setY(-1);
                auto v = projection * view * QVector4D(p, 1);
                if (v.w() <= 0)
                    continue;
                QPointF projected((v.x() / v.w() + 1) * width() / 2, (1 - v.y() / v.w()) * height() / 2);
                auto delta = projected - QPointF(point);
                const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
                if (distance < best) {
                    best = distance;
                    pick = {int(t), int(b)};
                }
            }
        selected = pick;
        requestUpdate();
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
    std::unique_ptr<QOpenGLShaderProgram> program, label_program, shadow_program;
    GLuint shadow_framebuffer=0, shadow_texture=0, shadow_color=0;
    bool shadow_attempted=false, shadow_ready=false;
    std::unique_ptr<QOpenGLTexture> label_texture;
    QOpenGLBuffer label_buffer{QOpenGLBuffer::VertexBuffer};
    QString label_signature;
    std::vector<float> vertices;
    int count = 0, capacity = 0, edge_capacity = 0;
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
        glDrawArrays(GL_TRIANGLES,0,count);
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
            "model3d; uniform bool reverseAmplitude; uniform bool floorPass; uniform vec2 floorSigns; varying float height; varying vec2 cell; varying vec3 surfaceNormal; varying vec3 worldPosition; "
            "void main(){vec3 p=vertex; if(floorPass)p.xz*=floorSigns; worldPosition=p; height=reverseAmplitude ? (1.0-vertex.y)/2.0 : (vertex.y+1.0)/2.0;cell=vertex.xz;surfaceNormal=model3d ? normal : "
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
            const size_t b0=bands*ranges[2]/100,b1=bands*ranges[3]/100;
            const size_t t0=size_t(std::ceil(scene.xMin)),t1=std::min(history,size_t(std::floor(scene.xMax))+1);
            setProperty("historyCapacity",qulonglong(history));setProperty("historyBands",qulonglong(bands));
            setProperty("validHistoryColumns",qulonglong(data.empty() ? 0 : std::min(history,data.front().size())));
            setProperty("sceneScaleX", scene.scaleX); setProperty("sceneScaleZ", scene.scaleZ);
            const size_t bs=std::max(size_t(1),(b1-b0)/256), ts=std::max(size_t(1),(t1-t0)/256);
            const size_t rows=b1>b0+1 ? (b1-b0-2)/bs+2 : 0;
            const size_t columns=t1>t0+1 ? (t1-t0-2)/ts+2 : 0;
            sample_vertices.resize(rows*columns);
            float peakValue=-std::numeric_limits<float>::infinity();size_t peakFrame=0,peakBand=0;
            for(size_t row=0;row<rows;++row) for(size_t column=0;column<columns;++column) {
                const size_t b=std::min(b0+row*bs,b1-1), t=std::min(t0+column*ts,t1-1);
                const float value=sample(b,t);const auto p=scene.point(t,b,value);
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
            if(rows>1 && columns>1) for(size_t row=0;row+1<rows;++row) for(size_t column=0;column+1<columns;++column) {
                vertex(row,column);vertex(row+1,column);vertex(row,column+1);
                vertex(row,column+1);vertex(row+1,column);vertex(row+1,column+1);
            }
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
            for(GLuint cell=0;cell<GLuint(count/6);++cell) {
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
            std::vector<float> floor;
            auto line = [&](QVector3D a, QVector3D b) {
                floor.insert(floor.end(), {a.x(),a.y(),a.z(),0,1,0,b.x(),b.y(),b.z(),0,1,0});
            };
            // Like Qt, put the two walls behind the surface for the current camera quadrant.
            const float sx=scene.scaleX+.1f, sz=scene.scaleZ+.1f;
            for (int tick=0;tick<=5;++tick) {
                const float f=tick/5.f, x=-sx+2*sx*f, z=-sz+2*sz*f, y=-1.1f+2.2f*f;
                line({x,-1.1f,-sz},{x,-1.1f,sz});
                line({-sx,-1.1f,z},{sx,-1.1f,z});
                line({-sx,y,-sz},{sx,y,-sz}); line({-sx,-1.1f,z},{-sx,1.1f,z});
                line({x,-1.1f,-sz},{x,1.1f,-sz}); line({-sx,y,-sz},{-sx,y,sz});
            }
            auto face = [&](QVector3D a,QVector3D b,QVector3D c,QVector3D d) {
                for (auto p : {a,b,c,a,c,d}) floor.insert(floor.end(),{p.x(),p.y(),p.z(),0,1,0});
            };
            face({-sx,-1.1f,-sz},{sx,-1.1f,-sz},{sx,-1.1f,sz},{-sx,-1.1f,sz});
            face({-sx,-1.1f,-sz},{sx,-1.1f,-sz},{sx,1.1f,-sz},{-sx,1.1f,-sz});
            face({-sx,-1.1f,-sz},{-sx,-1.1f,sz},{-sx,1.1f,sz},{-sx,1.1f,-sz});
            floor_buffer.bind(); floor_buffer.allocate(floor.data(),int(floor.size()*sizeof(float))); floor_buffer.release();
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
        glDrawArrays(GL_TRIANGLES, 0, count);
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
        floor_buffer.bind();
        program->setUniformValue("floorPass", true);
        const float wallX=position.x()>=0 ? 1.f : -1.f, wallZ=position.z()>=0 ? 1.f : -1.f;
        program->setUniformValue("floorSigns", QVector2D(wallX,wallZ));
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
            glDrawArrays(GL_TRIANGLES,72,18); glDisable(GL_POLYGON_OFFSET_FILL);
            program->setUniformValue("gridColor",QVector3D(grid_color.redF(),grid_color.greenF(),grid_color.blueF()));
        }
        for(int tick=0;tick<6;++tick) {
            if(options["floor_grid"].toBool(true)) glDrawArrays(GL_LINES,tick*12,4);
            if(options["axis_grid"].toBool(true)) glDrawArrays(GL_LINES,tick*12+4,8);
        }
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
        for (int tick = 0; tick <= 5; ++tick) {
            const float f=tick/5.f;const int precision=options["axis_precision"].toInt(0);
            texts << QString::number(scene.timeTick(f),'f',precision)
                  << QString::number(scene.bandTick(f),'f',precision)
                  << QString::number(scene.signalTick(f),'f',precision);
        }
        texts << ui::text("Time, frames (X)") << ui::text("Frequency, Mel bands (Z)") << ui::text("Amplitude (Y)");
        const auto signature = texts.join('|') + font().toString() + theme.labelTextColor().name();
        if (signature != label_signature) {
            core::perf::Scope atlasTrace(core::perf::Stage::SpectrogramAtlas);
            const auto dpr = devicePixelRatioF();
            QImage atlas(qRound(768 * dpr), qRound(224 * dpr), QImage::Format_RGBA8888);
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
        QVector<QRectF> numericBounds;
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
                QTransform rotation;rotation.rotate(angle*180/3.141592653589793);
                auto bounds=rotation.mapRect(QRectF(0,0,QFontMetrics(font()).horizontalAdvance(texts[index]),QFontMetrics(font()).height()));
                bounds.translate((projected.x()/projected.w()+1)*width()/2+offset.x(),
                    (1-projected.y()/projected.w())*height()/2+offset.y()-QFontMetrics(font()).ascent()-1);
                numericBounds.append(bounds.adjusted(-4,-4,4,4));
                label_program->setUniformValue("anchor", anchor);
                label_program->setUniformValue("angle", angle);
                label_program->setUniformValue("offset", offset - QVector2D(0, QFontMetrics(font()).ascent() + 1));
                label_program->setUniformValue(
                    "region", QVector4D((index % 3) / 3.f, 1 - (index / 3 + 1) / 7.f, 1 / 3.f, 1 / 7.f));
                glDrawArrays(GL_TRIANGLES, 0, 6);
            };
            for (int tick = 0; tick <= 5; ++tick) {
                const float fraction = tick / 5.f, p = -1 + fraction * 2;
                label(tick * 3, {p*scene.scaleX, -1.1f, wallZ*(scene.scaleZ+.25f)}, {3, 14}, .523599f*wallX);
                label(tick * 3 + 1, {wallX*(scene.scaleX+.25f), -1.1f, -p*scene.scaleZ}, {3, 14}, -.523599f*wallZ);
                label(tick * 3 + 2, {-wallX*(scene.scaleX+.1f), -1+fraction*2, wallZ*(scene.scaleZ+.1f)}, {5, 0});
            }
            label(20, {-wallX*(scene.scaleX+.1f), 1.1f, wallZ*(scene.scaleZ+.1f)}, {5, -8});
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
        numericBounds.append(QRectF(width()-76,46,76,140));
        const auto centerProjection=projection*view*QVector4D(0,-1.1f,0,1);
        const QPointF sceneCenter=centerProjection.w()>0 ?
            QPointF((centerProjection.x()/centerProjection.w()+1)*width()/2,(1-centerProjection.y()/centerProjection.w())*height()/2) :
            QPointF(width()/2.,height()/2.);
        const std::array<QVector3D,2> axisAnchors{{{0,-1.1f,wallZ*(scene.scaleZ+.1f)},
            {wallX*(scene.scaleX+.1f),-1.1f,0}}};
        for(int axis=0;axis<2;++axis) {
            const auto projected=projection*view*QVector4D(axisAnchors[axis],1);if(projected.w()<=0) continue;
            const QPointF anchor((projected.x()/projected.w()+1)*width()/2,(1-projected.y()/projected.w())*height()/2);
            if(!QRectF(rect()).adjusted(-24,-24,24,24).contains(anchor)) continue;
            const QString title=texts[18+axis];const auto metrics=QFontMetrics(font());
            const auto bounds=spectrogramAxisTitleRect(anchor,anchor-sceneCenter,
                QSizeF(metrics.horizontalAdvance(title)+16,metrics.height()+8),QRectF(rect()).adjusted(4,4,-4,-4),numericBounds);
            painter.save();painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(theme.labelTextColor(),1));painter.drawLine(anchor,bounds.center());
            painter.setBrush(background);painter.drawRoundedRect(bounds,3,3);
            painter.drawText(bounds.adjusted(6,2,-6,-2),Qt::AlignCenter,title);painter.restore();
            numericBounds.append(bounds.adjusted(-4,-4,4,4));
        }
        if (data.empty())
            painter.drawText(QRect(12, 12, std::max(1, width() - 24), 60), Qt::AlignLeft | Qt::TextWordWrap,
                             ui::text("Waiting for audio. Check the source and connections in Data Flow."));
        // This legend shows the exact original Qt gradient stops and data scale.
        QLinearGradient legend(0, 174, 0, 54);
        auto displayStops=stops;
        if(scene.reverseAmplitude) {for(auto &stop:displayStops) stop.first=1-stop.first;std::reverse(displayStops.begin(),displayStops.end());}
        legend.setStops(displayStops);
        const int legend_x = width() - 64;
        painter.fillRect(QRect(legend_x, 54, 14, 120), legend);
        painter.drawRect(QRect(legend_x, 54, 14, 120));
        for (int tick = 0; tick <= 4; ++tick)
            painter.drawText(legend_x + 20, 178 - tick * 30,
                QString::number(scene.signalTick(tick/4.f),'f',options["axis_precision"].toInt(0)));
        if (selection && selected.x() >= 0 && selected.y() >= 0 && size_t(selected.y()) < bandSize() &&
            size_t(selected.x()) < historySize()) {
            setProperty("selectedAge",selected.x());setProperty("selectedBand",selected.y());
            setProperty("selectedValue",sample(selected.y(),selected.x()));
            const int precision=options["axis_precision"].toInt();
            const auto readout=ui::text("Time frame: %1 · Band: %2 · Value: %3")
                .arg(QString::number(selected.x(),'f',precision))
                .arg(QString::number(selected.y(),'f',precision))
                .arg(QString::number(sample(selected.y(),selected.x()),'f',precision));
            painter.drawText(QRect(12,8,std::max(1,width()-24),36),Qt::AlignLeft | Qt::AlignTop,
                             QFontMetrics(painter.font()).elidedText(readout,Qt::ElideRight,std::max(1,width()-24)));
            const auto projected=projection*view*QVector4D(scene.point(selected.x(),selected.y(),sample(selected.y(),selected.x())),1);
            setProperty("selectionMarkerVisible",false);
            if(projected.w()>0 && selected.x()>=scene.xMin && selected.x()<=scene.xMax &&
                selected.y()>=scene.zMin && selected.y()<=scene.zMax) {
                const QPointF center((projected.x()/projected.w()+1)*width()/2,(1-projected.y()/projected.w())*height()/2);
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
                const QRectF panel = spectrogramSlicePanel(size(),slicePosition);
                painter.save();
                painter.setClipRect(panel);
                setProperty("slicePanelRect",panel);
                const QColor panelColor=background;
                const QColor ink=spectrogramTextColor(panelColor);
                painter.fillRect(panel,panelColor);QPen framePen(ink);framePen.setCosmetic(true);painter.setPen(framePen);
                painter.drawRect(panel.adjusted(.5,.5,-.5,-.5));
                const auto plot = spectrogramSlicePlot(panel);
                const bool timeSlice=selection==2;
                const bool reverseSlice=timeSlice ? scene.reverseBands : scene.reverseTime;
                const int begin=int(std::ceil(timeSlice ? scene.zMin : scene.xMin));
                const int end=std::min(int(timeSlice ? bandSize() : historySize()),
                                       timeSlice ? int(std::ceil(scene.zMax)) : int(std::floor(scene.xMax))+1);
                painter.drawText(panel.adjusted(8,3,-8,-3), Qt::AlignTop,
                                  ui::text(timeSlice ? "Time Slice" : "Frequency Slice"));
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
                painter.drawText(QPointF(panel.left()+8,plot.top()+5),QString::number(scene.signalTick(1),'f',precision));
                painter.drawText(QPointF(panel.left()+8,plot.bottom()),QString::number(scene.signalTick(0),'f',precision));
                painter.drawText(QRectF(plot.left(),plot.bottom()+2,plot.width()/2,18),Qt::AlignLeft | Qt::AlignTop,
                    QString::number(reverseSlice ? end-1 : begin,'f',precision));
                painter.drawText(QRectF(plot.center().x(),plot.bottom()+2,plot.width()/2,18),Qt::AlignRight | Qt::AlignTop,
                    QString::number(reverseSlice ? begin : end-1,'f',precision));
                painter.drawText(QRectF(plot.left(),panel.bottom()-20,plot.width(),16),Qt::AlignCenter,
                    ui::text(timeSlice ? "Frequency, Mel bands (Z)" : "Time, frames (X)"));
                painter.save();painter.translate(panel.left()+14,plot.center().y());painter.rotate(-90);
                painter.drawText(QRectF(-plot.height()/2,-10,plot.height(),16),Qt::AlignCenter,ui::text("Amplitude (Y)"));painter.restore();
                painter.setBrush(Qt::NoBrush);painter.setPen(framePen);
                painter.drawRect(panel.adjusted(.5,.5,-.5,-.5));
                setProperty("sliceSampleCount",std::max(0,end-begin));
                painter.restore();
            }
        } else {setProperty("sliceSampleCount",0);setProperty("selectionMarkerVisible",false);}
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
    m_spectrogram_graph->options=surface_settings();
    m_spectrogram_graph->shadow_error=[this](const QString &message){emit surface_settings_error(message);};
    m_container = m_spectrogram_graph;
    m_container->installEventFilter(this);
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
        _pause = value;
        if (!value && m_spectrogram_graph)
            m_spectrogram_graph->setData(_spectrogram);
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
        m_spectrogram_graph->selection = value;
        m_spectrogram_graph->requestUpdate();
        emit presentation_changed(presentation_state());
    });
    connect(&ui::Theme::instance(), &ui::Theme::changed, m_spectrogram_graph,
            [this] { m_spectrogram_graph->requestUpdate(); });
    auto *axisXControls=group("Axis range X");
    auto *axisYControls=group("Axis range Y");
    auto *axisZControls=group("Axis range Z");
    auto *xAxis=new AxisControlWidget("Time, frames (X)",panel);
    auto *yAxis=new AxisControlWidget("Amplitude (Y)",panel);
    auto *zAxis=new AxisControlWidget("Frequency, Mel bands (Z)",panel);
    xAxis->setObjectName("spectrogramAxisX");yAxis->setObjectName("spectrogramAxisY");zAxis->setObjectName("spectrogramAxisZ");
    xAxis->set_bounds(0,100);yAxis->set_bounds(-1e6,1e6);zAxis->set_bounds(0,100);
    axisXControls->addWidget(xAxis);axisYControls->addWidget(yAxis);axisZControls->addWidget(zAxis);
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
        yAxis->set_state(options["y_min"].toDouble(),options["y_max"].toDouble(1),options["y_range_mode"]!="manual",options["reverse_amplitude"].toBool());
        zAxis->set_state(ranges[2],ranges[3],ranges[2]==0 && ranges[3]==100 && zAxis->auto_box()->isChecked(),options["reverse_bands"].toBool());
    };
    connect(this,&SpectrogramGraph::surface_settings_updated,panel,syncAxes);
    auto refreshExtent=[this,xAxis,zAxis] {
        xAxis->set_hint(ui::text("Range (%)")+"\n"+ui::text("History: %1 frames").arg(QString("%1 / %2")
            .arg(_spectrogram.empty() ? 0 : _spectrogram.front().size()).arg(m_spectrogram_graph->historySize())));
        zAxis->set_hint(ui::text("Range (%)")+"\n"+ui::text("Mel bands: %1").arg(m_spectrogram_graph->bandSize()));
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
    if(!_pause || _spectrogram.empty()) m_spectrogram_graph->setData(_spectrogram);
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
void SpectrogramGraph::set_history_dimensions(size_t capacity,size_t bands) {
    if(!capacity || !bands || capacity>16384 || bands>4096 || capacity*bands>1048576) return;
    if(capacity==_history_capacity && bands==_history_bands) return;
    _history_capacity=capacity;_history_bands=bands;
    if(m_spectrogram_graph) {
        m_spectrogram_graph->history_capacity=capacity;m_spectrogram_graph->configured_bands=bands;
        if(m_spectrogram_graph->selected.x()>=int(capacity) || m_spectrogram_graph->selected.y()>=int(bands))
            m_spectrogram_graph->selected={-1,-1};
        m_spectrogram_graph->upload=true;m_spectrogram_graph->requestUpdate();
    }
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
    return options["y_range_mode"]=="manual" ? QPair<float,float>{float(options["y_min"].toDouble()),float(options["y_max"].toDouble(1))}
                                            : QPair<float,float>{0,2};
}
void SpectrogramGraph::set_y_range(float minimum,float maximum) {
    if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < -1e6f || maximum > 1e6f || maximum-minimum < 1e-6f) return;
    auto options=surface_settings();options["y_range_mode"]="manual";options["y_min"]=minimum;options["y_max"]=maximum;
    set_surface_settings(options);
}
QJsonObject SpectrogramGraph::surface_settings() const { return SurfaceSettings::normalized(_surface_settings); }
void SpectrogramGraph::set_surface_settings(const QJsonObject &effective) {
    const auto previousY=y_range();
    _surface_settings=SurfaceSettings::normalized(effective);
    if(m_spectrogram_graph) {
        const bool changed=m_spectrogram_graph->options!=_surface_settings;
        const bool remap=m_spectrogram_graph->options["reverse_time"]!=_surface_settings["reverse_time"] ||
            m_spectrogram_graph->options["reverse_bands"]!=_surface_settings["reverse_bands"] ||
            m_spectrogram_graph->options["reverse_amplitude"]!=_surface_settings["reverse_amplitude"] ||
            m_spectrogram_graph->options["y_range_mode"]!=_surface_settings["y_range_mode"] ||
            m_spectrogram_graph->options["y_min"]!=_surface_settings["y_min"] ||
            m_spectrogram_graph->options["y_max"]!=_surface_settings["y_max"];
        m_spectrogram_graph->options=_surface_settings;
        if(remap) m_spectrogram_graph->upload=true;
        m_spectrogram_graph->gradient=_surface_settings["gradient"]=="black_yellow" ? 0 : 1;
        if(changed) m_spectrogram_graph->requestUpdate();
    }
    const auto currentY=y_range();if(previousY!=currentY) emit y_range_changed(currentY.first,currentY.second);
    emit surface_settings_updated(_surface_settings);
}
void SpectrogramGraph::update_frame_history(const std::vector<std::vector<float>> &history) {
    const size_t bands=history.empty() ? 0 : history.front().size();
    bool valid=history.size()<=16384 && bands<=4096 && history.size()*bands<=1048576;
    for(const auto &frame:history) {
        valid &= frame.size()==bands;
        for(float value:frame) valid &= std::isfinite(value);
    }
    if(!valid || history.empty()) {update({});return;}
    std::vector<std::vector<float>> transposed(bands,std::vector<float>(history.size()));
    for(size_t frame=0;frame<history.size();++frame)
        for(size_t band=0;band<bands;++band) transposed[band][frame]=history[frame][band];
    update(transposed);
}
void SpectrogramGraph::update(const std::vector<std::vector<float>> &value) {
    core::perf::Scope trace(core::perf::Stage::SpectrogramData);
    // Reject malformed snapshots atomically: the previous frame is cleared.
    size_t length = value.empty() ? 0 : value.front().size();
    bool valid = value.size() <= 4096 && length <= 16384 && value.size() * length <= 1048576;
    for (const auto &row : value) {
        valid &= row.size() == length;
        for (float sample : row)
            valid &= std::isfinite(sample);
    }
    const auto sanitized = valid ? value : std::vector<std::vector<float>>{};
    if (_spectrogram == sanitized)
        return;
    _spectrogram = sanitized;
    if(!_history_capacity && !sanitized.empty() && m_spectrogram_graph) {
        m_spectrogram_graph->history_capacity=std::max(m_spectrogram_graph->history_capacity,length);
        m_spectrogram_graph->configured_bands=sanitized.size();
    }
    if (m_spectrogram_graph && (!_pause || sanitized.empty()))
        m_spectrogram_graph->setData(_spectrogram);
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
            _orientation = candidate.normalized();
    } else if (state.contains("zoom")) {
        const float yaw = state.value("horizontal").toDouble(), pitch = state.value("vertical").toDouble(30),
                    zoom = state.value("zoom").toDouble(100);
        if (std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(zoom)) {
            _orientation = QQuaternion::fromEulerAngles(-pitch, -yaw, 0);
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
    _position += _orientation.rotatedVector(move) * dt * _movement_speed * (_camera_keys.contains("fast") ? 2.5f : 1.f);
    if (_position.length() > 10)
        _position = _position.normalized() * 10;
    _camera_changed();
}
void SpectrogramGraph::_stop_camera() {
    _camera_keys.clear();
    _held_camera_keys.clear();
    _camera_timer.stop();
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
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate)
        _stop_camera();
    if(!_render_visible) return QWidget::eventFilter(watched,event);
    if (event->type() == QEvent::MouseButtonPress) {
        const auto *mouse=static_cast<QMouseEvent *>(event);
        _mouse_position = mouse->position().toPoint();
        m_container->setFocus();
        if(mouse->button()==Qt::LeftButton && !m_spectrogram_graph->beginSliceDrag(mouse->position()))
            m_spectrogram_graph->pick(_mouse_position);
        if(mouse->button()==Qt::LeftButton || mouse->button()==Qt::RightButton || mouse->button()==Qt::MiddleButton) return true;
    }
    if (event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if(m_spectrogram_graph->sliceDragging && (mouse->buttons() & Qt::LeftButton)) {
            m_spectrogram_graph->moveSlice(mouse->position());return true;
        }
        if (mouse->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
            auto delta = mouse->position().toPoint() - _mouse_position;
            _mouse_position = mouse->position().toPoint();
            if(mouse->buttons() & Qt::MiddleButton)
                _position+=_orientation.rotatedVector(QVector3D(-delta.x(),delta.y(),0))*.005f;
            else _orientation = (QQuaternion::fromAxisAndAngle({0, 1, 0}, -delta.x() * .3f) * _orientation *
                            QQuaternion::fromAxisAndAngle({1, 0, 0}, -delta.y() * .3f))
                               .normalized();
            _camera_changed();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && m_spectrogram_graph->sliceDragging) {
        m_spectrogram_graph->sliceDragging=false;
        emit presentation_changed(presentation_state());return true;
    }
    if (event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        _position += _orientation.rotatedVector({0, 0, -wheel->angleDelta().y() / 600.f});
        if (_position.length() > 10)
            _position = _position.normalized() * 10;
        _camera_changed();
        return true;
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
        if (key->modifiers() & (Qt::AltModifier | Qt::MetaModifier))
            return false;
        const int scan = CameraControls::scan(*key),
                  identity = scan ? scan : CameraControls::logical(key->key()) + 0x10000;
        if (key->isAutoRepeat())
            return true;
        if (event->type() == QEvent::KeyRelease && _held_camera_keys.remove(identity)) {
            _camera_keys.clear();
            for (const auto &action : _held_camera_keys)
                _camera_keys.insert(action);
            if (_camera_keys.isEmpty())
                _camera_timer.stop();
            return true;
        }
        for (auto it = _camera_controls.begin(); it != _camera_controls.end(); ++it)
            for (const auto &binding : it.value().toArray())
                if (CameraControls::matches(binding.toObject(), *key)) {
                    if (event->type() == QEvent::KeyPress) {
                        if (it.key() == "reset")
                            reset_camera();
                        else {
                            _held_camera_keys[identity] = it.key();
                            _camera_keys.insert(it.key());
                            if (!_camera_timer.isActive()) {
                                _camera_clock.start();
                                _camera_timer.start();
                            }
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
        state["slice_position"] = QJsonArray{m_spectrogram_graph->slicePosition.x(),m_spectrogram_graph->slicePosition.y()};
        state["ranges"] = QJsonArray{m_spectrogram_graph->ranges[0], m_spectrogram_graph->ranges[1],
                                     m_spectrogram_graph->ranges[2], m_spectrogram_graph->ranges[3]};
        state["selection"] = m_spectrogram_graph->selection;
        state["pause"] = _pause;
    }
    return state;
}
void SpectrogramGraph::set_presentation_state(const QJsonObject &state) {
    _presentation_state = state;
    if (!m_spectrogram_graph)
        return;
    const auto slicePosition=state.value("slice_position").toArray();
    if(slicePosition.size()==2) {
        const double x=slicePosition[0].toDouble(1),y=slicePosition[1].toDouble(1);
        if(std::isfinite(x) && std::isfinite(y))
            m_spectrogram_graph->slicePosition={std::clamp(x,0.0,1.0),std::clamp(y,0.0,1.0)};
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
    _pause = state.value("pause").toBool(false);
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
