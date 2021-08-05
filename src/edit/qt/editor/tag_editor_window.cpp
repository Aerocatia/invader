// SPDX-License-Identifier: GPL-3.0-only

#include <QCloseEvent>
#include <QMessageBox>
#include <QMenuBar>
#include <QScrollArea>
#include <QLabel>
#include <QScrollBar>
#include <QApplication>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <filesystem>
#include <invader/file/file.hpp>
#include "../tree/tag_tree_window.hpp"
#include "widget/tag_editor_widget.hpp"
#include "widget/tag_editor_edit_widget_view.hpp"
#include "../tree/tag_tree_dialog.hpp"
#include "subwindow/tag_editor_subwindow.hpp"
#include "subwindow/tag_editor_bitmap_subwindow.hpp"
#ifndef DISABLE_AUDIO
#include "subwindow/tag_editor_sound_subwindow.hpp"
#endif
#include "subwindow/tag_editor_string_subwindow.hpp"
#include "subwindow/tag_editor_font_subwindow.hpp"

using namespace Invader::Parser;

namespace Invader::EditQt {
    // Used for scrolling to a specific item
    class GotoAction : public QAction {
    public:
        void on_triggered(bool) {
            parent_window->scroll_to(this->text().toLatin1().data());
        }
        GotoAction(const char *text, TagEditorWindow *parent_window) : QAction(text), parent_window(parent_window) {
            connect(this, &QAction::triggered, this, &GotoAction::on_triggered);
        }
    private:
        TagEditorWindow *parent_window;
    };
    
    TagEditorWindow::TagEditorWindow(QWidget *parent, TagTreeWindow *parent_window, const File::TagFile &tag_file) : QMainWindow(parent), parent_window(parent_window), file(tag_file) {
        // If we're loading an existing tag, open it and parse it
        if(tag_file.tag_path.size() != 0) {
            this->make_dirty(false);
            auto open_file = File::open_file(tag_file.full_path);
            if(!open_file.has_value()) {
                char formatted_error[1024];
                std::snprintf(formatted_error, sizeof(formatted_error), "Failed to open %s.\n\nMake sure it exists and you have permission to open it.", tag_file.full_path.string().c_str());
                QMessageBox(QMessageBox::Icon::Critical, "Error", formatted_error, QMessageBox::Ok).exec();
                this->close();
                return;
            }
            try {
                this->parser_data = Parser::ParserStruct::parse_hek_tag_file(open_file->data(), open_file->size(), false).release();
            }
            catch(std::exception &e) {
                char formatted_error[1024];
                std::snprintf(formatted_error, sizeof(formatted_error), "Failed to open %s due to an exception error:\n\n%s", tag_file.full_path.string().c_str(), e.what());
                QMessageBox(QMessageBox::Icon::Critical, "Error", formatted_error, QMessageBox::Ok).exec();
                this->close();
                return;
            }

            if(this->parser_data->check_for_broken_enums(false)) {
                char formatted_error[1024];
                std::snprintf(formatted_error, sizeof(formatted_error), "Failed to parse %s due to enumerators being out-of-bounds.\n\nThe tag appears to be corrupt.", tag_file.full_path.string().c_str());
                QMessageBox(QMessageBox::Icon::Critical, "Error", formatted_error, QMessageBox::Ok).exec();
                this->close();
                return;
            }
        }
        else {
            this->make_dirty(true);
            this->parser_data = Parser::ParserStruct::generate_base_struct(tag_file.tag_fourcc).release();
            if(!this->parser_data) {
                char formatted_error[1024];
                std::snprintf(formatted_error, sizeof(formatted_error), "Failed to create a %s.", tag_fourcc_to_extension(tag_file.tag_fourcc));
                QMessageBox(QMessageBox::Icon::Critical, "Error", formatted_error, QMessageBox::Ok).exec();
                this->close();
                return;
            }
        }
        int min_width = this->minimumSizeHint().width();

        // Make and set our menu bar
        QMenuBar *bar = new QMenuBar(this);
        this->setMenuBar(bar);

        // File menu
        auto *file_menu = bar->addMenu("File");

        auto *save = file_menu->addAction("Save");
        save->setIcon(QIcon::fromTheme(QStringLiteral("document-save")));
        save->setShortcut(QKeySequence::Save);
        connect(save, &QAction::triggered, this, &TagEditorWindow::perform_save);

        auto *save_as = file_menu->addAction("Save as...");
        save_as->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as")));
        save_as->setShortcut(QKeySequence::SaveAs);
        connect(save_as, &QAction::triggered, this, &TagEditorWindow::perform_save_as);

        file_menu->addSeparator();

        auto *close = file_menu->addAction("Close");
        close->setShortcut(QKeySequence::Close);
        close->setIcon(QIcon::fromTheme(QStringLiteral("document-close")));
        connect(close, &QAction::triggered, this, &TagEditorWindow::close);

        int min_height = this->minimumSizeHint().height();

        // Add another widget to our view?
        QFrame *extra_widget_panel = nullptr;
        QPushButton *extra_widget;
        switch(tag_file.tag_fourcc) {
            case TagFourCC::TAG_FOURCC_BITMAP:
                extra_widget = new QPushButton("Preview bitmap");
                break;
            #ifndef DISABLE_AUDIO
            case TagFourCC::TAG_FOURCC_SOUND:
                extra_widget = new QPushButton("Preview sound");
                break;
            #endif
            case TagFourCC::TAG_FOURCC_FONT:
                extra_widget = new QPushButton("Preview font");
                break;
            case TagFourCC::TAG_FOURCC_STRING_LIST:
            case TagFourCC::TAG_FOURCC_UNICODE_STRING_LIST:
                extra_widget = new QPushButton("Preview string list");
                break;
            // case TagFourCC::TAG_FOURCC_GBXMODEL:
            // case TagFourCC::TAG_FOURCC_SCENARIO_STRUCTURE_BSP:
            //     extra_widget = new QPushButton("Preview model");
            //     break;
            // case TagFourCC::TAG_FOURCC_INVADER_UNIT_HUD_INTERFACE:
            // case TagFourCC::TAG_FOURCC_INVADER_WEAPON_HUD_INTERFACE:
            // case TagFourCC::TAG_FOURCC_INVADER_UI_WIDGET_DEFINITION:
            // case TagFourCC::TAG_FOURCC_UNIT_HUD_INTERFACE:
            // case TagFourCC::TAG_FOURCC_WEAPON_HUD_INTERFACE:
            //     extra_widget = new QPushButton("Preview interface");
            //     break;
            // case TagFourCC::TAG_FOURCC_SHADER:
            // case TagFourCC::TAG_FOURCC_SHADER_MODEL:
            // case TagFourCC::TAG_FOURCC_SHADER_ENVIRONMENT:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_CHICAGO:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_CHICAGO_EXTENDED:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_GENERIC:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_GLASS:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_METER:
            // case TagFourCC::TAG_FOURCC_SHADER_TRANSPARENT_WATER:
            //     extra_widget = new QPushButton("Preview shader");
            //     break;
            // case TagFourCC::TAG_FOURCC_SCENARIO:
            //     extra_widget = new QPushButton("Edit scenario");
            //     break;
            default:
                extra_widget = nullptr;
                break;
        }
        if(extra_widget) {
            extra_widget_panel = new QFrame();
            QHBoxLayout *extra_layout = new QHBoxLayout();
            extra_widget_panel->setLayout(extra_layout);
            extra_layout->addWidget(extra_widget);
            extra_layout->setContentsMargins(4, 4, 4, 4);
            connect(extra_widget, &QPushButton::clicked, this, &TagEditorWindow::show_subwindow);
        }

        // Set up the scroll area and widgets
        auto values = std::vector<Parser::ParserStructValue>(this->parser_data->get_values());
        this->scroll_widget = new QScrollArea();
        this->setCentralWidget(this->scroll_widget);
        this->main_widget = new TagEditorEditWidgetView(nullptr, values, this, true, extra_widget_panel);
        this->scroll_widget->setWidget(this->main_widget);

        // Goto menu (goto top level reflexives)
        auto *goto_menu = bar->addMenu("Goto");
        goto_menu->setEnabled(false);
        for(auto &v : values) {
            auto type = v.get_type();
            if(type == Parser::ParserStructValue::ValueType::VALUE_TYPE_REFLEXIVE || type == Parser::ParserStructValue::ValueType::VALUE_TYPE_GROUP_START) {
                goto_menu->addAction(new GotoAction(v.get_name(), this));
                goto_menu->setEnabled(true);
            }
        }

        // View menu
        auto *view_menu = bar->addMenu("View");
        auto *toggle_fullscreen = view_menu->addAction("Toggle Full Screen");
        toggle_fullscreen->setShortcut(QKeySequence::FullScreen);
        toggle_fullscreen->setIcon(QIcon::fromTheme(QStringLiteral("view-fullscreen")));
        connect(toggle_fullscreen, &QAction::triggered, this, &TagEditorWindow::toggle_fullscreen);

        // Figure out how big we want to make this window
        auto screen_geometry = QGuiApplication::primaryScreen()->geometry();
        int max_width = this->scroll_widget->widget()->width() + qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent) * 2 + min_width;
        int max_height = min_height + this->scroll_widget->widget()->height() + qApp->style()->pixelMetric(QStyle::PM_DefaultFrameWidth) * 2;
        this->scroll_widget->setWidgetResizable(true);

        if(max_height > screen_geometry.height() / 5 * 4) {
            max_height = screen_geometry.height() / 5 * 4;
        }

        if(max_width > screen_geometry.width() / 5 * 4) {
            max_width = screen_geometry.width() / 5 * 4;
        }

        // Center this
        this->setGeometry(
            QStyle::alignedRect(
                Qt::LeftToRight,
                Qt::AlignCenter,
                QSize(max_width, max_height),
                screen_geometry
            )
        );

        // We did it!
        this->successfully_opened = true;
    }
    
    void TagEditorWindow::scroll_to(const char *item) {
        int offset = this->main_widget->y_for_item(item);
        if(offset >= 0) {
            this->scroll_widget->verticalScrollBar()->setValue(offset);
        }
    }

    void TagEditorWindow::closeEvent(QCloseEvent *event) {
        bool accept;
        if(dirty) {
            char message_entire_text[512];
            if(this->file.tag_path.size() == 0) {
                std::snprintf(message_entire_text, sizeof(message_entire_text), "This is a new %s file.\nDo you want to save your changes?", tag_fourcc_to_extension(this->file.tag_fourcc));
            }
            else {
                std::snprintf(message_entire_text, sizeof(message_entire_text), "This file \"%s\" has been modified.\nDo you want to save your changes?", this->file.full_path.string().c_str());
            }
            QMessageBox are_you_sure(QMessageBox::Icon::Question, "Unsaved changes", message_entire_text, QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            switch(are_you_sure.exec()) {
                case QMessageBox::Save:
                    accept = this->perform_save();
                    break;
                case QMessageBox::Cancel:
                    accept = false;
                    break;
                case QMessageBox::Discard:
                    accept = true;
                    break;
                default:
                    std::terminate();
            }
        }
        else {
            accept = true;
        }

        event->setAccepted(accept);
        
        // If we denied closing the window, stop
        if(!accept) {
            return;
        }

        // Delete subwindow
        if(this->subwindow) {
            this->subwindow->deleteLater();
            this->subwindow = nullptr;
        }

        // Clean up
        this->parent_window->cleanup_windows(this);
    }

    bool TagEditorWindow::perform_save() {
        if(this->file.tag_path.size() == 0) {
            return this->perform_save_as();
        }

        // Save; benchmark
        auto start = std::chrono::steady_clock::now();
        auto tag_data = parser_data->generate_hek_tag_data();
        auto str = this->file.full_path.string();
        const auto *c_str = str.c_str();
        auto result = Invader::File::save_file(c_str, tag_data);
        if(!result) {
            char formatted_error[1024];
            std::snprintf(formatted_error, sizeof(formatted_error), "Failed to save %s.\n\nMake sure you have permission here.", c_str);
            QMessageBox(QMessageBox::Icon::Critical, "Error", formatted_error, QMessageBox::Ok).exec();
            this->close();
        }
        else {
            this->make_dirty(false);
            auto end = std::chrono::steady_clock::now();
            std::printf("Saved %s in %zu ms\n", this->get_file().full_path.string().c_str(), std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        }
        return result;
    }

    bool TagEditorWindow::perform_save_as() {
        TagTreeDialog d(nullptr, this->parent_window, this->file.tag_fourcc, std::filesystem::path(this->get_file().tag_path).parent_path().string().c_str());
        if(d.exec() == QMessageBox::Accepted) {
            this->file = *d.get_tag();
            
            std::error_code ec;
            std::filesystem::create_directories(this->file.full_path.parent_path(), ec);
            
            // Save it!
            auto result = this->perform_save();
            this->file = this->parent_window->all_tags.emplace_back(this->file);
            this->parent_window->reload_tags(false);
            
            // Done
            return result;
        }

        return false;
    }

    void TagEditorWindow::make_dirty(bool dirty) {
        this->dirty = dirty;

        // Start writing the title
        std::string title;
        if(this->file.tag_path.empty()) {
            title = std::string("Untitled ") + tag_fourcc_to_extension(this->file.tag_fourcc);
        }
        else {
            title = std::string(this->file.full_path.string().c_str()) + (dirty ? " *" : "");
        }
        
        title += std::string(" — ") + qApp->applicationDisplayName().toStdString();
        this->setWindowTitle(title.c_str());
        
        if(this->subwindow) {
            if(this->subwindow->isHidden()) {
                this->subwindow->deleteLater();
                this->subwindow = nullptr;
            }
            else {
                this->subwindow->setWindowTitle(this->file.tag_path.c_str());
                this->subwindow->update();
            }
        }
    }

    const File::TagFile &TagEditorWindow::get_file() const noexcept {
        return this->file;
    }

    void TagEditorWindow::toggle_fullscreen() {
        if(this->isFullScreen()) {
            this->showNormal();
        }
        else {
            this->showFullScreen();
        }
    }

    TagEditorWindow::~TagEditorWindow() {
        if(this->subwindow) {
            this->subwindow->deleteLater();
            this->subwindow = nullptr;
        }
        delete this->parser_data;
    }

    void TagEditorWindow::show_subwindow() {
        if(!this->subwindow) {
            switch(this->file.tag_fourcc) {
                case TagFourCC::TAG_FOURCC_BITMAP:
                    this->subwindow = new TagEditorBitmapSubwindow(this);
                    break;
                #ifndef DISABLE_AUDIO
                case TagFourCC::TAG_FOURCC_SOUND:
                    this->subwindow = new TagEditorSoundSubwindow(this);
                    break;
                #endif
                case TagFourCC::TAG_FOURCC_FONT:
                    this->subwindow = new TagEditorFontSubwindow(this);
                    break;
                case TagFourCC::TAG_FOURCC_STRING_LIST:
                case TagFourCC::TAG_FOURCC_UNICODE_STRING_LIST:
                    this->subwindow = new TagEditorStringSubwindow(this);
                    break;
                default:
                    std::terminate();
            }
            this->subwindow->show();
        }
        
        this->subwindow->setVisible(true);
        
        // Run all the memes to get this to the front
        this->subwindow->setFocus();
        this->subwindow->setWindowState((this->subwindow->windowState() | Qt::WindowState::WindowActive) & ~Qt::WindowMinimized);
        this->subwindow->raise();
        this->subwindow->activateWindow();
    }
}
