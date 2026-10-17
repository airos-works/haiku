/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_CONVERTER_WINDOW_H
#define MEDIA_CONVERTER_WINDOW_H


#include <Window.h>

#include "OutputSettings.h"


class BButton;
class BFilePanel;
class BMenuItem;
class BStatusBar;
class BStringView;
class HardwareWindow;
class JobProcess;
class OutputView;
class QueueItem;
class QueueView;
class SourceView;


class MediaConverterWindow : public BWindow {
public:
								MediaConverterWindow();
	virtual						~MediaConverterWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_AddRefs(BMessage* message);
			void				_RemoveSelected();
			void				_ClearFinished();
			void				_SelectionChanged();
			void				_StartProbing(BList* items);
			void				_StartPreview(QueueItem* item);

			void				_Convert();
			void				_StartConverting();
			const OutputSettings& _RunSettingsFor(QueueItem* item) const;
			bool				_StartNext();
			void				_JobFinished(BMessage* message);
			BString				_DestinationFor(QueueItem* item,
									const OutputSettings& settings);
			void				_UpdateControls();
			void				_UpdateStatus();

			void				_LoadSettings();
			void				_SaveSettings();

	static	status_t			_ProbeThread(void* data);
	static	status_t			_PreviewThread(void* data);
	static	status_t			_ConvertThread(void* data);

			QueueView*			fQueue;
			SourceView*			fSource;
			OutputView*			fOutput;
			BButton*			fAddButton;
			BButton*			fRemoveButton;
			BButton*			fConvertButton;
			BButton*			fStopButton;
			BStatusBar*			fProgress;
			BStringView*		fStatus;
			BMenuItem*			fWarnItem;
			BFilePanel*			fOpenPanel;
			BFilePanel*			fFolderPanel;
			HardwareWindow*		fHardwareWindow;

			bool				fConverting;
			bool				fStopping;
			bool				fWarnCpu;
			// what the conversion started with: for films and for songs
			OutputSettings		fRunSettings[2];
			JobProcess*			fProcess;
			thread_id			fThread;
			int32				fCurrentID;
			int32				fJobsDone;
			int32				fJobsTotal;
			BString				fLastOutput;
			bool				fConvertWhenReady;
};


#endif	// MEDIA_CONVERTER_WINDOW_H
