#include <QLineEdit>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraImageCapture>
#include <QTimer>

#include "app/arbiter.hpp"
#include "app/session.hpp"
#include "app/window.hpp"

#include "app/pages/camera.hpp"

/////////////////////////
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
/////////////////////////


#include <QPalette>
#include <QSerialPortInfo>

#include "app/config.hpp"
#include "canbus/elm327.hpp"
#include "plugins/vehicle_plugin.hpp"





CameraPage::CameraPage(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , Page(arbiter, "Camera", "camera", true, this)
    , arbiter(arbiter)
{
    initializeLayout();
    // Connect to page change signal
    connect(&arbiter, &Arbiter::pageChanged, this, [this](int pageId) {
        // Only take action if crossing the page 3 boundary
        if (pageId == 2 && previousPageId != 2) {
            //g_usleep(200000);
            connect_cam();
        } else if (previousPageId == 2 && pageId != 2) {
            disconnect_stream();
        }
        previousPageId = pageId;  // Update previous state
    });
}

void CameraPage::init()
{
    // Initialize member variables
    this->player = new QMediaPlayer(this);
    this->local_cam = nullptr;
    this->local_index = 0;
    this->reconnect_timer = new QTimer(this);
    this->config = Config::get_instance();
    this->videoContainer_ = nullptr;
    this->videoWidget_ = nullptr;

    for (auto device : QCanBus::instance()->availableDevices("socketcan"))
        this->can_devices.append(device.name());

    for (auto port : QSerialPortInfo::availablePorts())
        this->serial_devices.append(port.systemLocation());
    
    connect(this->reconnect_timer, &QTimer::timeout, this, &CameraPage::count_down);

    // Setup main layout structure
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Left container setup (80%)
    QWidget *leftContainer = new QWidget(this);
    leftContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    QStackedLayout *leftLayout = new QStackedLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(this->connect_widget());
    leftLayout->addWidget(this->local_camera_widget());

    // Right panel setup (20%)
    QWidget *rightPanel = new QWidget(this);
    //rightPanel->setStyleSheet("background-color:rgb(12, 12, 12);");
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);  // Add this to match

    // Create horizontal layout for settings button
    QHBoxLayout *settingsLayout = new QHBoxLayout();
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(0);
    settingsLayout->addStretch();  // This pushes the button to the right





    // Plugin container setup
    plugin_container = new QWidget(rightPanel);
    plugin_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    //plugin_container->setMinimumWidth(800);
    
    QVBoxLayout* plugin_layout = new QVBoxLayout(plugin_container);
    plugin_layout->setContentsMargins(0, 0, 0, 0);
    plugin_layout->setSpacing(0);

    // Dialog setup
    this->get_plugins();
    this->active_plugin = new QPluginLoader(this);
    Dialog *dialog = new Dialog(this->arbiter, true, this->window());
    dialog->set_body(this->dialog_body());
    QPushButton *load_button = new QPushButton("load");
    connect(load_button, &QPushButton::clicked, [this]() { this->load_plugin(); });
    dialog->set_button(load_button);

    // Setup settings button
    QPushButton *settings_button = new QPushButton(this);
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [dialog]() { dialog->open(); });

    // Add settings button to its container
    settingsLayout->addWidget(settings_button);

    // Assemble right panel
    rightLayout->addLayout(settingsLayout);
    rightLayout->addStretch();
    rightLayout->addWidget(plugin_container);
    
    

    // Assemble main layout
    mainLayout->addWidget(leftContainer, 80);
    mainLayout->addWidget(rightPanel, 20);

    // Connect signals
    connect(this, &CameraPage::disconnected, [leftLayout, this]{
        leftLayout->setCurrentIndex(0);
        this->disconnect_stream();
    });
    
    connect(this, &CameraPage::connected_local, [leftLayout, this]{
        leftLayout->setCurrentIndex(1);
        this->connected = false;
    });

    // Setup camera overlay
    if (QFile::exists("/tmp/dash_camera_overlay.svg")) {
        QFile::remove("/tmp/dash_camera_overlay.svg");
    }
    QFile::copy(":/camera_overlay.svg", "/tmp/dash_camera_overlay.svg");

    // Load initial plugin
    this->load_plugin();
}

void CameraPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    DASH_LOG(info) << "[CameraPage] Show event.";
    if (this->config->get_cam_autoconnect() && (this->connected == false))
        this->connect_cam();
}

void CameraPage::init_gstreamer_pipeline(std::string desc, bool sync)
{
    videoWidget_ = new QQuickWidget(videoContainer_);
    videoWidget_->setClearColor(QColor(18, 18, 18));
    videoWidget_->rootContext()->setContextProperty(QLatin1String("videoSurface"), surface_);
    videoWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);

    GError *error = nullptr;
    std::string pipeline = desc;
    //
    if (this->config->get_cam_overlay()) {
        double width = this->config->get_cam_overlay_width() / 100.0;
        double height = this->config->get_cam_overlay_height() / 100.0;
        double x = (1 - width) / 2.0;
        double y = (1 - height);
        pipeline = pipeline +
                   " ! videoconvert ! rsvgoverlay location=/tmp/dash_camera_overlay.svg width-relative=" + std::to_string(width) + " height-relative=" + std::to_string(height) + " x-relative=" + std::to_string(x) + " y-relative=" + std::to_string(y);
    }
    pipeline = pipeline +
                " ! capsfilter caps=video/x-raw name=mycapsfilter";

    
    // Get plane ID from DRM
    int planeId = 93; // Default fallback
    int fd = ::open("/dev/dri/card0", O_RDWR);  // Use global open() function
    if (fd >= 0) {
        drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
        if (planes && planes->count_planes > 1) {
            drmModePlane* plane = drmModeGetPlane(fd, planes->planes[1]);
            if (plane) {
                planeId = plane->plane_id;
                drmModeFreePlane(plane);
            }
            drmModeFreePlaneResources(planes);
        }
        ::close(fd);  // Also use global close()
    }            
    
    GstElement* kmssink = gst_element_factory_make("kmssink", "kmssink");
    g_object_set(G_OBJECT(kmssink), "plane-id", planeId, nullptr);
    g_object_set(G_OBJECT(kmssink), "bus-id", "display-subsystem", nullptr);
    g_object_set(G_OBJECT(kmssink), "skip-vsync", true , nullptr);

    DASH_LOG(info) << "[CameraPage] Created GStreamer Pipeline of `" << pipeline << "`";
    vidPipeline_ = gst_parse_launch(pipeline.c_str(), &error);
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(vidPipeline_));
    gst_bus_add_watch(bus, (GstBusFunc)&CameraPage::busCallback, this);
    gst_object_unref(bus);

    GstElement* capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    gst_bin_add(GST_BIN(vidPipeline_), kmssink);
    gst_element_link(capsFilter, kmssink);
}

QWidget *CameraPage::connect_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);

    QPushButton *settings_button = new QPushButton(this);
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);

    QHBoxLayout *layout2 = new QHBoxLayout();
    layout2->setContentsMargins(0, 0, 0, 0);
    layout2->setSpacing(0);
    layout2->addStretch();
    layout2->addWidget(settings_button);
    layout->addLayout(layout2);
    QLabel *label = new QLabel("Connect Camera", widget);

    this->status = new QLabel(widget);

    QWidget *cam_stack_widget = new QWidget(widget);
    QStackedLayout *cam_stack = new QStackedLayout(cam_stack_widget);
    cam_stack->addWidget(this->local_cam_selector());
    cam_stack->addWidget(this->network_cam_selector());

    QCheckBox *auto_reconnect_toggle = new QCheckBox("Automatically Reconnect", this);
    auto_reconnect_toggle->setLayoutDirection(Qt::RightToLeft);
    auto_reconnect_toggle->setChecked(this->config->get_cam_autoconnect());
    connect(auto_reconnect_toggle, &QCheckBox::toggled, [this](bool checked){
        this->config->set_cam_autoconnect(checked);
        if (!checked) emit autoconnect_disabled(); });
    connect(this, &CameraPage::autoconnect_disabled, [auto_reconnect_toggle, this]{
        this->reconnect_timer->stop();
        this->config->set_cam_autoconnect(false);
        auto_reconnect_toggle->setChecked(false);
    });

    QCheckBox *network_toggle = new QCheckBox("Network", this);
    connect(network_toggle, &QCheckBox::toggled, [this, cam_stack](bool checked){
        cam_stack->setCurrentIndex(checked ? 1 : 0);
        this->status->clear();
        this->config->set_cam_is_network(checked);
    });
    network_toggle->setChecked(this->config->get_cam_is_network());

    QWidget *checkboxes_widget = new QWidget(this);
    QHBoxLayout *checkboxes = new QHBoxLayout(checkboxes_widget);
    checkboxes->addWidget(network_toggle);
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
    dialog->set_body(new CameraPage::Settings(this->arbiter, this));
    connect(settings_button, &QPushButton::clicked, [dialog]{ dialog->open(); });

    return widget;
}

CameraPage::VideoContainer::VideoContainer(QWidget *parent, CameraPage *page) : QWidget(parent)
{
    this->page = page;
}

void CameraPage::VideoContainer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if ((this->page->videoWidget_ != nullptr) && (this->page->videoContainer_ != nullptr))
        this->page->videoWidget_->resize(event->size());
    DASH_LOG(info) << "[CameraPage] videoContainer resized";
}

CameraPage::Settings::Settings(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
{
    this->config = Config::get_instance();
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 0);

    layout->addWidget(this->settings_widget());
}

QSize CameraPage::Settings::sizeHint() const
{
    int label_width = QFontMetrics(this->font()).averageCharWidth() * 21;
    return QSize(label_width * 2, this->height() + 12);
}

QWidget *CameraPage::Settings::settings_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->addWidget(this->camera_overlay_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addWidget(this->camera_overlay_width_row_widget(), 1);
    layout->addWidget(this->camera_overlay_height_row_widget(), 1);

    return widget;
}

QWidget *CameraPage::Settings::camera_overlay_row_widget()
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

QWidget *CameraPage::Settings::camera_overlay_width_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Overlay Width");
    layout->addWidget(label, 1);

    layout->addWidget(this->camera_overlay_width_widget(), 1);

    return widget;
}

QWidget *CameraPage::Settings::camera_overlay_width_widget()
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

QWidget *CameraPage::Settings::camera_overlay_height_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Overlay Height");
    layout->addWidget(label, 1);

    layout->addWidget(this->camera_overlay_height_widget(), 1);

    return widget;
}

QWidget *CameraPage::Settings::camera_overlay_height_widget()
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

QWidget *CameraPage::local_camera_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
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

    this->local_video_widget = new CameraPage::VideoContainer(widget, this);

    layout->addWidget(this->local_video_widget);

    return widget;
}

QWidget *CameraPage::network_camera_widget()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QPushButton *disconnect = new QPushButton(widget);
    disconnect->setFlat(true);
    connect(disconnect, &QPushButton::clicked, [this]{
        emit autoconnect_disabled();
        emit disconnected();
    });
    this->arbiter.forge().iconize("close", disconnect, 16);
    layout->addWidget(disconnect, 0, Qt::AlignRight);

    this->remote_video_widget = new CameraPage::VideoContainer(widget, this);
    layout->addWidget(this->remote_video_widget);

    return widget;
}

QPushButton *CameraPage::connect_button()
{
    QPushButton *connect_button = new QPushButton("connect", this);
    connect_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect_button->setFlat(true);
    connect(connect_button, &QPushButton::clicked, this, &CameraPage::connect_cam);

    return connect_button;
}

void CameraPage::count_down()
{
    this->reconnect_in_secs--;
    this->status->setText(this->reconnect_message.arg(this->reconnect_in_secs) + ((this->reconnect_in_secs == 1) ? "second" : "seconds"));
    if (this->reconnect_in_secs == 0)
        this->connect_cam();
}

void CameraPage::connect_cam()
{
    this->reconnect_timer->stop();
    this->status->clear();
    if (this->config->get_cam_is_network())
        this->connect_network_stream();
    else
        this->connect_local_stream();
}

QWidget *CameraPage::local_cam_selector()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);

    QLabel *label = new QLabel(widget);
    label->setAlignment(Qt::AlignCenter);
    QWidget *selector = this->selector_widget(label);
    this->populate_local_cams();
    connect(this, &CameraPage::prev_cam, [this, label]{
        this->local_index = (this->local_index - 1 + this->local_cams.size()) % this->local_cams.size();
        auto cam = this->local_cams.at(local_index);
        label->setText(cam.first);
        this->config->set_cam_local_device(cam.second);
    });
    connect(this, &CameraPage::next_cam, [this, label]{
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

QWidget *CameraPage::selector_widget(QWidget *selection)
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QPushButton *left_button = new QPushButton(widget);
    left_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_left", left_button, 32);
    connect(left_button, &QPushButton::clicked, this, &CameraPage::prev_cam);

    QPushButton *right_button = new QPushButton(this);
    right_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_right", right_button, 32);
    connect(right_button, &QPushButton::clicked, this, &CameraPage::next_cam);

    layout->addStretch(1);
    layout->addWidget(left_button);
    layout->addWidget(selection, 2);
    layout->addWidget(right_button);
    layout->addStretch(1);

    return widget;
}

void CameraPage::populate_local_cams()
{
    this->local_cams.clear();
    this->local_index = 0;
    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (auto const &cam : cameras) {
        QString pretty_name = cam.description() + " at " + cam.deviceName();
        if (cam.description() == "AFN_CAP: AFN_CAP"){
            this->local_cams.append(QPair<QString, QString>(pretty_name, cam.deviceName())); 
            this->config->set_cam_local_device(cam.deviceName()); 
        }
    }
    QString default_device = this->config->get_cam_local_device();
    if (default_device.isEmpty() && !QCameraInfo::defaultCamera().isNull())
        default_device = QCameraInfo::defaultCamera().deviceName();




        
       //this->local_cams.append(QPair<QString, QString>(pretty_name, cam.deviceName()));
        //if (cam.deviceName() == default_device)

    //QString default_device = this->config->get_cam_local_device();
    //if (default_device.isEmpty() && !QCameraInfo::defaultCamera().isNull())
    //    default_device = QCameraInfo::defaultCamera().deviceName();
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

QWidget *CameraPage::network_cam_selector()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLineEdit *input = new QLineEdit(this->config->get_cam_network_url(), widget);
    input->setContextMenuPolicy(Qt::NoContextMenu);
    input->setFont(this->arbiter.forge().font(18));
    input->setAlignment(Qt::AlignCenter);
    connect(input, &QLineEdit::textEdited, [this](QString text){
        this->status->clear();
        this->config->set_cam_network_url(text);
        this->reconnect_timer->stop();
    });
    connect(input, &QLineEdit::cursorPositionChanged, [this](int, int){
        this->reconnect_timer->stop();
        this->status->clear(); });
    connect(input, &QLineEdit::returnPressed, this, &CameraPage::connect_network_stream);

    layout->addStretch(1);
    layout->addWidget(input, 4);
    layout->addStretch(1);

    return widget;
}

void CameraPage::connect_network_stream()
{
    videoContainer_ = this->remote_video_widget;

    DASH_LOG(info) << "[CameraPage] Creating GStreamer pipeline with " << this->config->get_cam_network_url().toStdString();
    std::string pipeline = "rtspsrc location=" + this->config->get_cam_network_url().toStdString() + " latency=300" +
                           " ! queue " +
                           // decodebin does some absolute magic on selecting proper plugins
                           // more importantly, it knows just the right settings to apply to these plugins
                           // I can recreate the exact same pipeline as it ends up using, and it will be much slower because of the lack of setting tuning.
                           " ! decodebin" + "";
    init_gstreamer_pipeline(pipeline, true);
    //emit the connected signal before we resize anything, so that videoContainer has had time to resize to the proper dimensions
    emit connected_network();
    if (videoContainer_ == nullptr) {
        DASH_LOG(info) << "[CameraPage] No video container, setting projection fullscreen";
        videoWidget_->setFocus();
        videoWidget_->setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
        videoWidget_->showFullScreen();
    }
    else {
        DASH_LOG(info) << "[CameraPage] Resizing to video container";
        videoWidget_->resize(videoContainer_->size());
        DASH_LOG(info) << "[CameraPage] Size: " << videoContainer_->width() << "x" << videoContainer_->height();
        videoWidget_->show();
    }

    GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    GstPad *convertPad = gst_element_get_static_pad(capsFilter, "sink");
    gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &CameraPage::convertProbe, this, nullptr);
    gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);
}

GstPadProbeReturn CameraPage::convertProbe(GstPad *pad, GstPadProbeInfo *info, void *)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (caps != nullptr) {
                GstVideoInfo *vinfo = gst_video_info_new();
                gst_video_info_from_caps(vinfo, caps);
                DASH_LOG(info) << "[CameraPage] Video Width: " << vinfo->width;
                DASH_LOG(info) << "[CameraPage] Video Height: " << vinfo->height;
            }

            return GST_PAD_PROBE_REMOVE;
        }
    }

    return GST_PAD_PROBE_OK;
}

void CameraPage::disconnect_stream()
{
    DASH_LOG(info) << "[CameraPage] Disconnecting camera and destroying gstreamer pipeline";
    //GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    //GstPad *convertPad = gst_element_get_static_pad(capsFilter, "sink");
    //gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &CameraPage::convertProbe, this, nullptr);
    gst_element_set_state(vidPipeline_, GST_STATE_NULL);
    g_object_unref(vidPipeline_);
    if (this->config->get_cam_autoconnect()) {
        qDebug() << "Camera disconnected. Auto reconnect in" << this->config->get_cam_autoconnect_time_secs() << "seconds";
        this->reconnect_message = this->status->text() + " - reconnecting in %1 ";
        this->reconnect_in_secs = this->config->get_cam_autoconnect_time_secs();
        //this->reconnect_timer->start(1000);
    }
    this->connected = false;
}

void CameraPage::connect_local_stream()
{
    this->videoContainer_ = this->local_video_widget;
    if (this->local_cam != nullptr) {
        delete this->local_cam;
        this->local_cam = nullptr;
    }
    const QString &local = this->config->get_cam_local_device();
    if (!this->local_cam_available(local)) {
        this->status->setText("camera unavailable");
        return;
    }
    this->local_cam = new QCamera(local.toUtf8(), this);
    this->local_cam->load();
    qDebug() << "camera status: " << this->local_cam->status();

    QSize res = this->choose_video_resolution();

    QSize screenSize = QGuiApplication::primaryScreen()->size();
    int x = 68;
    int y = -58;
    //int width = videoContainer_->width();
    //int height = videoContainer_->height();
    int width = 1480;
    int height = 836;
    //int width = 1480;
    //int height = 840;

    DASH_LOG(info) << "[CameraPage] Creating GStreamer pipeline with " << this->config->get_cam_local_device().toStdString();
    std::string pipeline = "v4l2src device=" + this->config->get_cam_local_device().toStdString() +
                           " ! capsfilter caps=\"video/x-raw,width=" + std::to_string(res.width()) + ",height=" + std::to_string(res.height()) + ";image/jpeg,width=" + std::to_string(res.width()) + ",height=" + std::to_string(res.height()) + "\"" +
                           " ! mppjpegdec ! mpph264enc ! h264parse ! mppvideodec format=BGRA";
    //" ! rsvgoverlay location=/tmp/dash_camera_overlay.svg" +
    //" ! capsfilter  name=mycapsfilter";
    //" ! kmssink plane-id=79 skip-vsync=true" +
    //" render-rectangle=\"<" +
    // std::to_string(x) + ", " +
    // std::to_string(y) + ", " +
    // std::to_string(width) + ", " +
    // std::to_string(height) + ">\"";
    init_gstreamer_pipeline(pipeline);
    //emit the connected signal before we resize anything, so that videoContainer has had time to resize to the proper dimensions
    emit connected_local();
    if (videoContainer_ == nullptr) {
        DASH_LOG(info) << "[CameraPage] No video container, setting projection fullscreen";
        videoWidget_->setFocus();
        videoWidget_->setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint);
        videoWidget_->showFullScreen();
    }
    else {
        DASH_LOG(info) << "[CameraPage] Resizing to video container";
        videoWidget_->resize(videoContainer_->size());
        DASH_LOG(info) << "[CameraPage] Size: " << videoContainer_->width() << "x" << videoContainer_->height();
        videoWidget_->show();
    }

    //GstElement *capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    //GstPad *convertPad = gst_element_get_static_pad(capsFilter, "sink");
    //gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &CameraPage::convertProbe, this, nullptr);
    GstElement* kmssink = gst_bin_get_by_name(GST_BIN(vidPipeline_), "kmssink");
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink),
        x,
        y,
        width,
        height);
    gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);
}

gboolean CameraPage::busCallback(GstBus *, GstMessage *message, gpointer *)
{
    gchar *debug;
    GError *err;
    gchar *name;

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(message, &err, &debug);
            DASH_LOG(info) << "[CameraPage] Error " << err->message;
            g_error_free(err);
            g_free(debug);
            break;
        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(message, &err, &debug);
            DASH_LOG(info) << "[CameraPage] Warning " << err->message << " | Debug " << debug;
            name = (gchar *)GST_MESSAGE_SRC_NAME(message);
            DASH_LOG(info) << "[CameraPage] Name of src " << (name ? name : "nil");
            g_error_free(err);
            g_free(debug);
            break;
        case GST_MESSAGE_EOS:
            DASH_LOG(info) << "[CameraPage] End of stream";
            break;
        case GST_MESSAGE_STATE_CHANGED:
        default:
            break;
    }

    return TRUE;
}

QSize CameraPage::choose_video_resolution()
{
    QSize window_size(1360, 768);
    //QSize window_size = this->size();
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

bool CameraPage::local_cam_available(const QString &device)
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
Gauge::Gauge(units_t units, QFont value_font, QFont unit_font, Gauge::Orientation orientation, int rate,
             std::vector<Command> cmds, int precision, obd_decoder_t decoder, QWidget *parent)
: QWidget(parent)
{
    Config *config = Config::get_instance();
    ICANBus *bus;
    switch(config->get_vehicle_can_bus()){
        //ELM327 USB
        case ICANBus::VehicleBusType::ELM327USB:
            bus = (ICANBus *)elm327::get_usb_instance();
            break;
        //ELM327 Bluetooth
        case ICANBus::VehicleBusType::ELM327BT:
            bus = (ICANBus *)elm327::get_bt_instance();
            break;
        //SocketCAN
        case ICANBus::VehicleBusType::SocketCAN:
        default:
            bus = (ICANBus *)SocketCANBus::get_instance();
            break;
    }

    using namespace std::placeholders;
    std::function<void(QByteArray)> callback = std::bind(&Gauge::can_callback, this, std::placeholders::_1);

    bus->registerFrameHandler(cmds[0].frame.frameId()+0x9, callback);
    DASH_LOG(info)<<"[Gauges] Registered frame handler for id "<<(cmds[0].frame.frameId()+0x9);

    this->si = config->get_si_units();

    this->rate = rate;
    this->precision = precision;

    this->cmds = cmds;
    this->decoder = decoder;

    QBoxLayout *layout;
    if (orientation == BOTTOM)
        layout = new QVBoxLayout(this);
    else
        layout = new QHBoxLayout(this);

    value_label = new QLabel(this->null_value(), this);
    value_label->setFont(value_font);
    value_label->setAlignment(Qt::AlignCenter);

    QLabel *unit_label = new QLabel(this->si ? units.second : units.first, this);
    unit_label->setFont(unit_font);
    unit_label->setAlignment(Qt::AlignCenter);

    this->timer = new QTimer(this);
    connect(this->timer, &QTimer::timeout, [this, bus, cmds]() {
        for (auto cmd : cmds) {
            bus->writeFrame(cmd.frame);
        }
    });

    connect(config, &Config::si_units_changed, [this, units, unit_label](bool si) {
        this->si = si;
        unit_label->setText(this->si ? units.second : units.first);
        value_label->setText(this->null_value());
    });

    layout->addStretch(6);
    layout->addWidget(value_label);
    layout->addStretch(1);
    layout->addWidget(unit_label);
    layout->addStretch(4);
}

void Gauge::can_callback(QByteArray payload){
    Response resp = Response(payload);
    for(auto cmd : cmds){
        if(cmd.frame.payload().at(2) == resp.PID){
            value_label->setText(this->format_value(this->decoder(cmd.decoder(resp), this->si)));
        }
    }
}

QString Gauge::format_value(double value)
{
    if (this->precision == 0)
        return QString::number((int)value);
    else
        return QString::number(value, 'f', this->precision);
}

QString Gauge::null_value()
{
    QString null_str = "-";
    if (this->precision > 0)
        null_str += ".-";
    else
        null_str += '-';

    return null_str;
}
*/

QWidget *CameraPage::dialog_body()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);

    QStringList plugins = this->plugins.keys();
    this->plugin_selector = new Selector(plugins, this->config->get_vehicle_plugin(), this->arbiter.forge().font(14), this->arbiter, widget, "unloader");

    layout->addWidget(this->si_units_row_widget(), 1);
    layout->addWidget(Session::Forge::br(), 1);
    layout->addWidget(this->can_bus_toggle_row(), 1);

    QStringList devices;
    switch(config->get_vehicle_can_bus()){
        //ELM327 USB
        case ICANBus::VehicleBusType::ELM327USB:
            devices = this->serial_devices;
            break;
        //ELM327 Bluetooth
        case ICANBus::VehicleBusType::ELM327BT:
            
            break;
        //SocketCAN
        case ICANBus::VehicleBusType::SocketCAN:
        default:
            devices = this->can_devices;
            break;
    }
   
    Selector *interface_selector = new Selector(devices, this->config->get_vehicle_interface(), this->arbiter.forge().font(14), this->arbiter, widget, "disabled");
    interface_selector->setVisible((this->can_devices.size() > 0) || (this->serial_devices.size() > 0) || (this->paired_bt_devices.size() > 0));
    connect(interface_selector, &Selector::item_changed, [this](QString item){
        if(this->config->get_vehicle_can_bus()==ICANBus::VehicleBusType::ELM327BT && item != QString("disabled"))
        {
            this->config->set_vehicle_interface(this->paired_bt_devices[item]);
        }
        else
        {
            this->config->set_vehicle_interface(item);
        }
    });
    connect(this->config, &Config::vehicle_can_bus_changed, [this, interface_selector](int state){
        switch(state){
            //ELM327 USB
            case ICANBus::VehicleBusType::ELM327USB:
                interface_selector->set_options(this->serial_devices);
                break;
            //ELM327 Bluetooth
            case ICANBus::VehicleBusType::ELM327BT:
                interface_selector->set_options(this->paired_bt_devices.keys());
                break;
            //SocketCAN
            case ICANBus::VehicleBusType::SocketCAN:
            default:
                interface_selector->set_options(this->can_devices);
                break;
        }
    });
    connect(&this->arbiter.system().bluetooth, &Bluetooth::init, [this, interface_selector]{
        interface_selector->setVisible((this->can_devices.size() > 0) || (this->serial_devices.size() > 0) || (this->paired_bt_devices.size() > 0));
        if(this->config->get_vehicle_can_bus()==ICANBus::VehicleBusType::ELM327BT){
            QString current = this->config->get_vehicle_interface();
            interface_selector->set_options(this->paired_bt_devices.keys());
            if(current != "disabled")
                interface_selector->set_current(this->paired_bt_devices.key(current));

        }
    });
    layout->addWidget(interface_selector, 1);

    layout->addWidget(Session::Forge::br(), 1);
    layout->addWidget(this->plugin_selector, 1);

    return widget;
}

QWidget *CameraPage::can_bus_toggle_row()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("Interface", widget);
    layout->addWidget(label, 1);

    QGroupBox *group = new QGroupBox();
    QVBoxLayout *group_layout = new QVBoxLayout(group);

    ICANBus::VehicleBusType can_bus_selected = this->config->get_vehicle_can_bus();
    QRadioButton *socketcan_button = new QRadioButton("SocketCAN", group);
    socketcan_button->setChecked(can_bus_selected==ICANBus::VehicleBusType::SocketCAN);
    socketcan_button->setEnabled(this->can_devices.size() > 0);
    connect(socketcan_button, &QRadioButton::clicked, [config = this->config]{
        config->set_vehicle_can_bus(ICANBus::VehicleBusType::SocketCAN);
    });
    group_layout->addWidget(socketcan_button);

    QRadioButton *elm_usb_button = new QRadioButton("ELM327 (USB)", group);
    elm_usb_button->setChecked(can_bus_selected==ICANBus::VehicleBusType::ELM327USB);
    elm_usb_button->setEnabled(this->serial_devices.size() > 0);
    connect(elm_usb_button, &QRadioButton::clicked, [config = this->config]{
        config->set_vehicle_can_bus(ICANBus::VehicleBusType::ELM327USB);
    });
    group_layout->addWidget(elm_usb_button);

    QRadioButton *elm_bt_button = new QRadioButton("ELM327 (Bluetooth)", group);
    elm_bt_button->setChecked(can_bus_selected==ICANBus::VehicleBusType::ELM327BT);
    elm_bt_button->setEnabled(false);
    connect(elm_bt_button, &QRadioButton::clicked, [config = this->config]{
        config->set_vehicle_can_bus(ICANBus::VehicleBusType::ELM327BT);
    });
    connect(&this->arbiter.system().bluetooth, &Bluetooth::init, [this, elm_bt_button]{
            elm_bt_button->setEnabled(this->paired_bt_devices.size() > 0);
    });
    group_layout->addWidget(elm_bt_button);

    layout->addWidget(group, 1, Qt::AlignHCenter);

    return widget;
}

QWidget *CameraPage::si_units_row_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QLabel *label = new QLabel("SI Units", widget);
    layout->addWidget(label, 1);

    Switch *toggle = new Switch(widget);
    toggle->scale(this->arbiter.layout().scale);
    toggle->setChecked(this->config->get_si_units());
    connect(toggle, &Switch::stateChanged, [config = this->config](bool state) { config->set_si_units(state); });
    layout->addWidget(toggle, 1, Qt::AlignHCenter);

    return widget;
}

void CameraPage::get_plugins()
{
    for (const QFileInfo &plugin : Session::plugin_dir("vehicle").entryInfoList(QDir::Files)) {
        if (QLibrary::isLibrary(plugin.absoluteFilePath()))
            this->plugins[Session::fmt_plugin(plugin.baseName())] = plugin;
    }
}

void CameraPage::load_plugin()
{
    // Clear existing plugin widgets
    QLayout* plugin_layout = plugin_container->layout();
    while (plugin_layout && plugin_layout->count() > 0) {
        QLayoutItem* item = plugin_layout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    if (this->active_plugin->isLoaded())
        this->active_plugin->unload();

    QString key = this->plugin_selector->get_current();
    if (!key.isNull()) {
        this->active_plugin->setFileName(this->plugins[key].absoluteFilePath());

        if (VehiclePlugin *plugin = qobject_cast<VehiclePlugin *>(this->active_plugin->instance())) {
            plugin->dashize(&this->arbiter);
            
            // Initialize plugin with appropriate CAN bus
            switch(config->get_vehicle_can_bus()) {
                case ICANBus::VehicleBusType::ELM327USB:
                    plugin->init((ICANBus *)elm327::get_usb_instance());
                    break;
                case ICANBus::VehicleBusType::ELM327BT:
                    plugin->init((ICANBus *)elm327::get_bt_instance());
                    break;
                case ICANBus::VehicleBusType::SocketCAN:
                default:
                    plugin->init((ICANBus *)SocketCANBus::get_instance());
                    break;
            }

            // Integrate plugin widgets directly
            integratePluginWidgets(plugin->widgets());
        }
    }
    this->config->set_vehicle_plugin(key);
}


void CameraPage::initializeLayout()
{
    main_layout = new QHBoxLayout();
    main_layout->addWidget(Session::Forge::br(true));
    // Add container for plugin widgets
    plugin_container = new QWidget(this);
    QVBoxLayout* plugin_layout = new QVBoxLayout(plugin_container);
    plugin_layout->setContentsMargins(0, 0, 0, 0);
    plugin_layout->setSpacing(0);
    main_layout->addWidget(Session::Forge::br(true));
    main_layout->addWidget(plugin_container);

}

void CameraPage::integratePluginWidgets(const QList<QWidget*>& widgets)
{
    QVBoxLayout* plugin_layout = qobject_cast<QVBoxLayout*>(plugin_container->layout());
    if (!plugin_layout) {
        return;
    }
    
    // Clear existing widgets
    while (plugin_layout->count() > 0) {
        QLayoutItem* item = plugin_layout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    
    // Add new widgets with expanding size policies
    for (QWidget* widget : widgets) {
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        policy.setHorizontalStretch(1);
        policy.setVerticalStretch(1);
        widget->setSizePolicy(policy);
        
        // Set minimum size for widgets to prevent them from becoming too small
        widget->setMinimumSize(375, 688);
        
        plugin_layout->addWidget(widget, 1);  // Add stretch factor to widget
    }
}
