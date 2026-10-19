#pragma once
#include <core_app/core_application.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <optional>
struct CoreApplication::StartupState {
    QString config_path,startup_error,load_error;
    bool existing_config=false,loaded=false,first_run_demo=false;
    domain::NumericProcessingGraph numeric_draft;
    QJsonObject runtime_config;
    std::optional<NumericExampleGraph> first_example;
};
struct CoreApplication::SaveState {
    QString path,target;
    bool recover_source=false;
    domain::NumericProcessingGraph draft;
    desktop::NumericDraftPresentation delta;
    std::optional<desktop::NumericSaveConsent> consent;
    std::unique_ptr<desktop::NumericProjectDocument> new_document;
};
