#ifndef ANNOTATIONSEDITORDOCKWIDGET_H
#define ANNOTATIONSEDITORDOCKWIDGET_H

#include "widgets/CutterDockWidget.h"
#include "Cutter.h"
#include "widgets/CutterSeekableWidget.h"

class MainWindow;
class AnnotationsEditorWidget;

class AnnotationsEditorDockWidget : public CutterDockWidget
{
  Q_OBJECT

  public:
      explicit AnnotationsEditorDockWidget(MainWindow *main, QAction *action = nullptr);
      ~AnnotationsEditorDockWidget();

  public slots:
    void onSeekChanged(RVA addr);

  private:
    AnnotationsEditorWidget *widget;
    CutterSeekableWidget *seekable;

};

#endif // ANNOTATIONSEDITORDOCKWIDGET_H
