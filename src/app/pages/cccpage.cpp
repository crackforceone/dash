#include <QLineEdit>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraImageCapture>
#include <QTimer>

/////////////////////////
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
/////////////////////////


#include "app/arbiter.hpp"
#include "app/session.hpp"
#include "app/window.hpp"

#include "app/pages/cccpage.hpp"

CCCPage::CCCPage(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , Page(arbiter, "CCC", "ccc", true, this)
    , arbiter(arbiter)
{
        // Connect to page change signal
        connect(&arbiter, &Arbiter::pageChanged, this, [this](int pageId) {
            // Only take action if crossing the page 3 boundary
            if (pageId == 3 && previousPageId != 3) {
                //g_usleep(200000);
                connect_cam();
            } else if (previousPageId == 3 && pageId != 3) {
                disconnect_stream();
            }
            previousPageId = pageId;  // Update previous state
        });
}

void CCCPage::init()
{
    this->player = new QMediaPlayer(this);
    this->local_cam = nullptr;
    this->local_index = 0;
    this->reconnect_timer = new QTimer(this);
    connect(this->reconnect_timer, &QTimer::timeout, this, &CCCPage::count_down);

    this->config = Config::get_instance();

    QStackedLayout *layout = new QStackedLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(this->connect_widget());
    layout->addWidget(this->local_camera_widget());

    connect(this, &CCCPage::disconnected, [layout, this]{
        layout->setCurrentIndex(0);
        this->disconnect_stream();
    });

    connect(this, &CCCPage::connected_local, [layout, this]{
        layout->setCurrentIndex(1);
        this->connected=true;
    });

    videoContainer_ = nullptr;
    videoWidget_ = nullptr;

    // Write camera_overlay to /tmp so that we can later point gstreamer to it
    if (QFile::exists("/tmp/dash_camera_overlay.svg"))
        QFile::remove("/tmp/dash_camera_overlay.svg");
    QFile::copy(":/camera_overlay.svg", "/tmp/dash_camera_overlay.svg");

    //connect_cam();
}

void CCCPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    DASH_LOG(info) << "[CCCPage] Show event.";
    if (this->config->get_cam_autoconnect() && (this->connected == false))
        this->connect_cam();
}

void CCCPage::init_gstreamer_pipeline(std::string desc, bool sync)
{
    videoWidget_ = new QQuickWidget(videoContainer_);
    videoWidget_->setClearColor(QColor(18, 18, 18));

    //surface_ = new QGst::Quick::VideoSurface;
    //videoWidget_->rootContext()->setContextProperty(QLatin1String("videoSurface"), surface_);
    //videoWidget_->setSource(QUrl("qrc:/camera_video.qml"));
    videoWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    

    //videoSink_ = surface_->videoSink();

    GError *error = nullptr;
    std::string pipeline = desc;
    DASH_LOG(info) << "[CCCPage] Created GStreamer Pipeline of `" << pipeline << "`";
    vidPipeline_ = gst_parse_launch(pipeline.c_str(), &error);
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(vidPipeline_));
    gst_bus_add_watch(bus, (GstBusFunc)&CCCPage::busCallback, this);
    gst_object_unref(bus);

    //GstElement *sink = QGlib::RefPointer<QGst::Element>(videoSink_);
    //g_object_set(sink, "force-aspect-ratio", false, nullptr);
    //g_object_set(sink, "sync", sync, nullptr);

    //g_object_set(sink, "async", false, nullptr);

    //GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    //gst_bin_add(GST_BIN(vidPipeline_), GST_ELEMENT(sink));
    //gst_element_link(capsFilter, GST_ELEMENT(sink));
}

QWidget *CCCPage::connect_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    //QPushButton *settings_button = new QPushButton(this);
    //settings_button->setFlat(true);
    //this->arbiter.forge().iconize("settings", settings_button, 24);

    QHBoxLayout *layout2 = new QHBoxLayout();
    layout2->setContentsMargins(0, 0, 0, 0);
    layout2->setSpacing(0);
    layout2->addStretch();
    //layout2->addWidget(settings_button);
    layout->addLayout(layout2);
    QLabel *label = new QLabel("Connect Camera", widget);

    this->status = new QLabel(widget);

    QWidget *cam_stack_widget = new QWidget(widget);
    QStackedLayout *cam_stack = new QStackedLayout(cam_stack_widget);
    cam_stack->addWidget(this->local_cam_selector());

    QCheckBox *auto_reconnect_toggle = new QCheckBox("Automatically Reconnect", this);
    auto_reconnect_toggle->setLayoutDirection(Qt::RightToLeft);
    auto_reconnect_toggle->setChecked(this->config->get_cam_autoconnect());
    connect(auto_reconnect_toggle, &QCheckBox::toggled, [this](bool checked){
        this->config->set_cam_autoconnect(checked);
        if (!checked) emit autoconnect_disabled(); });
    connect(this, &CCCPage::autoconnect_disabled, [auto_reconnect_toggle, this]{
        this->reconnect_timer->stop();
        this->config->set_cam_autoconnect(false);
        auto_reconnect_toggle->setChecked(false);
    });

    QWidget *checkboxes_widget = new QWidget(this);
    QHBoxLayout *checkboxes = new QHBoxLayout(checkboxes_widget);
    checkboxes->addStretch();
    checkboxes->addWidget(auto_reconnect_toggle);

    layout->addStretch();
    layout->addWidget(label, 0, Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(cam_stack_widget);
    layout->addWidget(this->status, 0, Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(this->connect_button(), 0, Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(checkboxes_widget);

    Dialog *dialog = new Dialog(this->arbiter, true, this->window());
    //dialog->set_body(new CCCPage::Settings(this->arbiter, this));
    //connect(settings_button, &QPushButton::clicked, [dialog]{ dialog->open(); });

    return widget;
}

CCCPage::VideoContainer::VideoContainer(QWidget *parent, CCCPage *page) : QWidget(parent)
{
    this->page = page;
}

void CCCPage::VideoContainer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if ((this->page->videoWidget_ != nullptr) && (this->page->videoContainer_ != nullptr))
        this->page->videoWidget_->resize(event->size());
    DASH_LOG(info) << "[CCCPage] videoContainer resized";
}

QWidget *CCCPage::local_camera_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 50, 0);
    layout->setSpacing(0);

    QPushButton *disconnect = new QPushButton(widget);
    disconnect->setFlat(true);
    connect(disconnect, &QPushButton::clicked, [this]{
        this->status->setText(QString());
        emit autoconnect_disabled();
        emit disconnected();
        this->local_cam = nullptr;
    });
    this->arbiter.forge().iconize("close", disconnect, 16);
    layout->addWidget(disconnect, 0, Qt::AlignRight);

    this->local_video_widget = new CCCPage::VideoContainer(widget, this);

    layout->addWidget(this->local_video_widget);

    return widget;
}

QPushButton *CCCPage::connect_button()
{
    QPushButton *connect_button = new QPushButton("connect", this);
    connect_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect_button->setFlat(true);
    connect(connect_button, &QPushButton::clicked, this, &CCCPage::connect_cam);

    return connect_button;
}

void CCCPage::count_down()
{
    this->reconnect_in_secs--;
    this->status->setText(this->reconnect_message.arg(this->reconnect_in_secs) + ((this->reconnect_in_secs == 0) ? "second" : "seconds"));
    if (this->reconnect_in_secs == 0)
        this->connect_cam();
}

void CCCPage::connect_cam()
{
    this->reconnect_timer->stop();
    this->status->clear();
    this->connect_local_stream();
}

QWidget *CCCPage::local_cam_selector()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);

    QLabel *label = new QLabel(widget);
    label->setAlignment(Qt::AlignCenter);
    QWidget *selector = this->selector_widget(label);
    this->populate_local_cams();
    connect(this, &CCCPage::prev_cam, [this, label]{
        this->local_index = (this->local_index - 1 + this->local_cams.size()) % this->local_cams.size();
        auto cam = this->local_cams.at(local_index);
        label->setText(cam.first);
        this->config->set_cam_local_device(cam.second);
    });
    connect(this, &CCCPage::next_cam, [this, label]{
        this->local_index = (this->local_index + 1) % this->local_cams.size();
        auto cam = this->local_cams.at(this->local_index);
        label->setText(cam.first);
        this->config->set_cam_local_device(cam.second);
    });



    label->setText(this->local_cams.at(local_index).first);
    layout->addWidget(selector);

    QHBoxLayout *refresh_row = new QHBoxLayout();
    QPushButton *refresh_button = new QPushButton(widget);
    refresh_button->setFlat(true);
    this->arbiter.forge().iconize("refresh", refresh_button, 16);

    refresh_row->addStretch(1);
    refresh_row->addWidget(refresh_button);
    refresh_row->addStretch(1);
    layout->addLayout(refresh_row);
    connect(refresh_button, &QPushButton::clicked, this, [this, label]{
        this->populate_local_cams();
        label->setText(this->local_cams.at(local_index).first);
    });

    return widget;
}

QWidget *CCCPage::selector_widget(QWidget *selection)
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QPushButton *left_button = new QPushButton(widget);
    left_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_left", left_button, 32);
    connect(left_button, &QPushButton::clicked, this, &CCCPage::prev_cam);

    QPushButton *right_button = new QPushButton(this);
    right_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_right", right_button, 32);
    connect(right_button, &QPushButton::clicked, this, &CCCPage::next_cam);

    layout->addStretch(1);
    layout->addWidget(left_button);
    layout->addWidget(selection, 2);
    layout->addWidget(right_button);
    layout->addStretch(1);

    return widget;
}

void CCCPage::populate_local_cams()
{
    this->local_cams.clear();
    this->local_index = 0;
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (auto const &cam : cameras) {
        QString pretty_name = cam.description() + " at " + cam.deviceName();
        //if (cam.description() == "UGREEN Camera: UGREEN Camera"){ //orig
        if (cam.description() == "ESP UVC Device: UVC CAM1"){
            this->local_cams.append(QPair<QString, QString>(pretty_name, cam.deviceName())); 
            this->config->set_cam_ccc_device(cam.deviceName()); 
        }
                
            //DASH_LOG(info) << "populate " << pretty_name;    
            //funktioniert. Es wird nur die Webcam angezeigt
    }
    
    QString default_device = this->config->get_cam_local_device();
    if (default_device.isEmpty() && !QCameraInfo::defaultCamera().isNull())
        default_device = QCameraInfo::defaultCamera().deviceName();
        
    /*
    int i = 0;
    for (auto const &cam : cameras) {
        QString pretty_name = cam.description() + " at " + cam.deviceName();
        this->local_cams.append(QPair<QString, QString>(pretty_name, cam.deviceName()));
        if (cam.deviceName() == default_device)
            this->local_index = i;
        i++;
    }
    */
    if (this->local_cams.isEmpty())
        this->local_cams.append(QPair<QString, QString>(QString("<No local cameras found>"), QString()));




}

GstPadProbeReturn CCCPage::convertProbe(GstPad *pad, GstPadProbeInfo *info, void *)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (caps != nullptr) {
                GstVideoInfo *vinfo = gst_video_info_new();
                gst_video_info_from_caps(vinfo, caps);
                DASH_LOG(info) << "[CCCPage] Video Width: " << vinfo->width;
                DASH_LOG(info) << "[CCCPage] Video Height: " << vinfo->height;
            }

            return GST_PAD_PROBE_REMOVE;
        }
    }

    return GST_PAD_PROBE_OK;
}

void CCCPage::disconnect_stream()
{
    DASH_LOG(info) << "[CCCPage] Disconnecting camera and destroying gstreamer pipeline";
    //GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    //GstPad *convertPad = gst_element_get_static_pad(capsFilter, "sink");
    //gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &CCCPage::convertProbe, this, nullptr);
    gst_element_set_state(vidPipeline_, GST_STATE_NULL);
    g_object_unref(vidPipeline_);
    if (this->config->get_cam_autoconnect()) {
        qDebug() << "Camera disconnected. Auto reconnect in" << this->config->get_cam_autoconnect_time_secs() << "seconds";
        this->reconnect_message = this->status->text() + " - reconnecting in %1 ";
        this->reconnect_in_secs = this->config->get_cam_autoconnect_time_secs();
        //this->reconnect_timer->start(500);
    }
    this->connected = false;
}

void CCCPage::connect_local_stream()
{
    this->videoContainer_ = this->local_video_widget;
    if (this->local_cam != nullptr) {
        delete this->local_cam;
        this->local_cam = nullptr;
    }

    //DASH_LOG(info) << "populate " << this->local_cam;

    const QString &local = this->config->get_cam_ccc_device();
    if (!this->local_cam_available(local)) {
        this->status->setText("camera unavailable");
        return;
    }
    //DASH_LOG(info) << "conf camtest 1 " << this->config->get_cam_ccc_device().toStdString();
    this->local_cam = new QCamera(local.toUtf8(), this);
    this->local_cam->load();
    qDebug() << "camera status: " << this->local_cam->status();

    QSize res = this->choose_video_resolution();
    if (!res.isValid()) {
        // Fallback to the known-good CLI resolution so caps are never 0x0.
        res = QSize(400, 240);
    }


    QSize screenSize = QGuiApplication::primaryScreen()->size();
    //int x = (screenSize.width() - res.width()) / 2;
    int x = 380;
    int y = 0;
    //int width = res.width(); //orig
    //int height = res.height(); /orig
    int width = 1200;
    int height = 720;

    // Use the plane that works for CLI testing; the DRM probe above was landing on
    // an unusable plane and kmssink would never present video.
    int planeId = 79;



    DASH_LOG(info) << "[CCCPage] Creating GStreamer pipeline with " << this->config->get_cam_ccc_device().toStdString();
    std::string pipeline = "v4l2src device=" + this->config->get_cam_ccc_device().toStdString() +
                           " ! image/jpeg,width=" + std::to_string(res.width()) +
                           ",height=" + std::to_string(res.height()) + ",framerate=15/1" +
                           " ! mppjpegdec ! mpph264enc ! h264parse ! mppvideodec format=RGB" +
                           " ! kmssink plane-id=" + std::to_string(planeId) +
                           " bus-id=display-subsystem skip-vsync=true" +
                           " render-rectangle=\"<" +
                            std::to_string(x) + ", " +
                            std::to_string(y) + ", " +
                            std::to_string(width) + ", " +
                            std::to_string(height) + ">\"";

    init_gstreamer_pipeline(pipeline);
    //emit the connected signal before we resize anything, so that videoContainer has had time to resize to the proper dimensions
    emit connected_local();
    if (videoContainer_ == nullptr) {
        DASH_LOG(info) << "[CCCPage] No video container, setting projection fullscreen";
        videoWidget_->setFocus();
        videoWidget_->setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
        videoWidget_->showFullScreen();
    }
    else {
        DASH_LOG(info) << "[CCCPage] Resizing to video container";
        videoWidget_->resize(videoContainer_->size());
        DASH_LOG(info) << "[CCCPage] Size: " << videoContainer_->width() << "x" << videoContainer_->height();
        videoWidget_->show();
    }

    //GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    //GstPad *convertPad = gst_element_get_static_pad(capsFilter, "sink");
    //gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &CCCPage::convertProbe, this, nullptr);
    GstStateChangeReturn stateRet = gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);
    if (stateRet == GST_STATE_CHANGE_FAILURE) {
        DASH_LOG(error) << "[CCCPage] Failed to set pipeline to PLAYING";
        this->status->setText("Pipeline failed to start");
    }
}

gboolean CCCPage::busCallback(GstBus *, GstMessage *message, gpointer *)
{
    gchar *debug;
    GError *err;
    gchar *name;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(message, &err, &debug);
            DASH_LOG(info) << "[CCCPage] Error " << err->message;
            g_error_free(err);
            g_free(debug);
            break;
        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(message, &err, &debug);
            DASH_LOG(info) << "[CCCPage] Warning " << err->message << " | Debug " << debug;
            name = (gchar *)GST_MESSAGE_SRC_NAME(message);
            DASH_LOG(info) << "[CCCPage] Name of src " << (name ? name : "nil");
            g_error_free(err);
            g_free(debug);
            break;
        case GST_MESSAGE_EOS:
            DASH_LOG(info) << "[CCCPage] End of stream";
            break;
        case GST_MESSAGE_STATE_CHANGED:
        default:
            break;
    }

    return TRUE;
}

QSize CCCPage::choose_video_resolution()
{
    //QSize window_size(1280, 720); //orig
    QSize window_size(400, 240);
    QCameraImageCapture imageCapture(this->local_cam);
    int min_gap = 10000, xgap, ygap;
    QSize max_fit;
    qDebug() << "camera: " << this->local_cam;

    qDebug() << "resolutions: " << imageCapture.supportedResolutions();
    for (auto const &resolution : imageCapture.supportedResolutions()) {
        xgap = window_size.width() - resolution.width();
        ygap = window_size.height() - resolution.height();
        if (xgap >= 0 && ygap >= 0 && xgap + ygap < min_gap) {
            min_gap = xgap + ygap;
            max_fit = resolution;
        }
    }
    if (max_fit.isValid()) {
        qDebug() << "Local cam auto resolution" << max_fit << "to fit in" << window_size;
        this->local_cam_settings.setResolution(max_fit);
    }
    else {
        qDebug() << "No suitable resolutions found to fit in" << window_size;
    }

    if (this->config->get_cam_local_format_override() > 0) {
        this->local_cam_settings.setPixelFormat(this->config->get_cam_local_format_override());
        qDebug() << "Overriding local cam format to" << this->config->get_cam_local_format_override();
    }
    this->local_cam->setViewfinderSettings(this->local_cam_settings);
    return max_fit;
}

bool CCCPage::local_cam_available(const QString &device)
{
    
    if (device.isEmpty())
        return false;

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo : cameras) {
        if (cameraInfo.deviceName() == device)
            return true;
    }
    return false;
}



/*
CCCPage::Settings::Settings(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
{
    this->config = Config::get_instance();
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 0);

    layout->addWidget(this->settings_widget());
}

QSize CCCPage::Settings::sizeHint() const
{
    int label_width = QFontMetrics(this->font()).averageCharWidth() * 21;
    return QSize(label_width * 2, this->height() + 12);
}

QWidget *CCCPage::Settings::settings_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->addWidget(this->camera_overlay_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addWidget(this->camera_overlay_width_row_widget(), 1);
    layout->addWidget(this->camera_overlay_height_row_widget(), 1);

    return widget;
}

QWidget *CCCPage::Settings::camera_overlay_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Backup Camera Overlay");
    layout->addWidget(label, 1);

    Switch *toggle = new Switch();
    toggle->scale(this->arbiter.layout().scale);
    toggle->setChecked(this->config->get_cam_overlay());
    connect(toggle, &Switch::stateChanged, [config = this->config](bool state) { config->set_cam_overlay(state); });
    layout->addWidget(toggle, 1, Qt::AlignHCenter);

    return widget;
}

QWidget *CCCPage::Settings::camera_overlay_width_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Overlay Width");
    layout->addWidget(label, 1);

    layout->addWidget(this->camera_overlay_width_widget(), 1);

    return widget;
}

QWidget *CCCPage::Settings::camera_overlay_width_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QSlider *slider = new QSlider(Qt::Orientation::Horizontal);
    slider->setTracking(false);
    slider->setRange(0, 150);
    slider->setValue(this->config->get_cam_overlay_width());
    QLabel *value = new QLabel(QString::number(slider->value()));
    connect(slider, &QSlider::valueChanged, [config = this->config, value](int position){
        config->set_cam_overlay_width(position);
        value->setText(QString::number(position));
    });
    connect(slider, &QSlider::sliderMoved, [value](int position){
        value->setText(QString::number(position));
    });

    layout->addStretch(2);
    layout->addWidget(slider, 4);
    layout->addWidget(value, 2);

    return widget;
}

QWidget *CCCPage::Settings::camera_overlay_height_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Overlay Height");
    layout->addWidget(label, 1);

    layout->addWidget(this->camera_overlay_height_widget(), 1);

    return widget;
}

QWidget *CCCPage::Settings::camera_overlay_height_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QSlider *slider = new QSlider(Qt::Orientation::Horizontal);
    slider->setTracking(false);
    slider->setRange(0, 150);
    slider->setValue(this->config->get_cam_overlay_height());
    QLabel *value = new QLabel(QString::number(slider->value()));
    connect(slider, &QSlider::valueChanged, [config = this->config, value](int position){
        config->set_cam_overlay_height(position);
        value->setText(QString::number(position));
    });
    connect(slider, &QSlider::sliderMoved, [value](int position){
        value->setText(QString::number(position));
    });

    layout->addStretch(2);
    layout->addWidget(slider, 4);
    layout->addWidget(value, 2);

    return widget;
}
*/