// SPDX-License-Identifier: GPL-3.0-only

#ifndef INVADER__EDIT__QT__TAG_EDITOR_WINDOW_HPP
#define INVADER__EDIT__QT__TAG_EDITOR_WINDOW_HPP

#include <QMainWindow>
#include <memory>
#include <invader/file/file.hpp>
#include "../tree/tag_tree_widget.hpp"

class QScrollArea;

namespace Invader::Parser {
    struct ParserStruct;
}

namespace Invader::EditQt {
    class TagTreeWindow;
    class TagEditorSubwindow;
    class TagEditorEditWidgetView;
    class TagEditorSoundSubwindow;

    class TagEditorWindow : public QMainWindow {
        friend class TagEditorWidget;
        friend class TagEditorSoundSubwindow;
        friend class TagTreeWindow;

        Q_OBJECT
    public:
        /**
         * Instantiate a TagFileDialog
         * @param parent         parent widget
         * @param parent_window  parent window
         */
        TagEditorWindow(QWidget *parent, TagTreeWindow *parent_window, const File::TagFile &file);

        /**
         * Close event
         * @param event event pointer
         */
        virtual void closeEvent(QCloseEvent *event);

        /**
         * Get the currently open tag file
         * @return tag file
         */
        const File::TagFile &get_file() const noexcept;

        /**
         * Get the parent window
         * @return parent window
         */
        TagTreeWindow *get_parent_window() noexcept {
            return this->parent_window;
        }

        /**
         * Get whether or not this opened successfully
         * @return opened successfully
         */
        bool is_successfully_opened() const noexcept {
            return this->successfully_opened;
        }

        /**
         * Get the parser data
         * @return parser data
         */
        Parser::ParserStruct *get_parser_data() noexcept {
            return this->parser_data;
        }
        
        /**
         * Scroll to the given item
         */
        void scroll_to(const char *item);

        ~TagEditorWindow();

    private:
        TagTreeWindow *parent_window;

        bool dirty = false;

        void make_dirty(bool dirty);

        bool perform_save();
        bool perform_save_as();
        File::TagFile file;

        Parser::ParserStruct *parser_data = nullptr;
        std::vector<std::unique_ptr<QWidget>> widgets_to_remove;
        TagEditorSubwindow *subwindow = nullptr;

        bool successfully_opened = false;
        
        QScrollArea *scroll_widget;
        TagEditorEditWidgetView *main_widget;

        void toggle_fullscreen();
        void show_subwindow();
    };
}

#endif
