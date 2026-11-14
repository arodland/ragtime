#ifndef RAGTIME_THRESHOLDS_H
#define RAGTIME_THRESHOLDS_H

#include <wx/arrstr.h>
#include <wx/string.h>

#include "ragtime/gearbox/Submodes.h"

// Where the gearshift's thresholds come from (docs/GEARBOX.md, "Where
// Ragtime gets the figures"). Each source adds to or updates the one before:
//
// 1. data2g-host's own data2g/codes_data/mode_thresholds.json, found in a
//    Data2G checkout in one of the usual folders (findData2GThresholds).
// 2. The file picked with Load thresholds... (Preferences key
//    /Gearshift/ThresholdsFile), else submode_thresholds.json in Ragtime's
//    data folder.
//
// Widths and speeds then come from the host's MODES list, and from hosts
// since Data2G PR #186 the thresholds too, winning over the files'.

// The file Ragtime looks for when none has been picked: submode_thresholds.json
// in Ragtime's data folder, appDataDir() (~/.local/share/ragtime on Linux,
// %APPDATA%\Ragtime on Windows).
wxString defaultThresholdsPath();

// The picked file, else the default one if it exists. Empty when there is
// neither.
wxString thresholdsPath();
void rememberThresholdsPath(const wxString& path);

// data2g-host's mode_thresholds.json in a Data2G checkout: $DATA2G_HOME,
// then Data2G or data2g in the home folder or in its src, git, code,
// projects, repos or dev folder. Empty when none is found.
wxString findData2GThresholds();
// The folders findData2GThresholds() looks in, for the popup to show.
wxArrayString data2gSearchFolders();

// Reads a thresholds file into *table (adding to what is there). False with
// a reason in *error.
bool loadThresholds(const wxString& path, ragtime::gearbox::SubmodeTable& table,
                    wxString& error);

// The table from every source in order. *sources names each file used;
// *errors gathers files that failed to read (one per line), and is empty
// when all went well.
ragtime::gearbox::SubmodeTable loadAllThresholds(wxArrayString& sources, wxString& errors);
// The files the last loadAllThresholds() used.
wxArrayString loadedThresholdSources();

#endif
