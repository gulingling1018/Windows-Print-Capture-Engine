#include <QApplication>
#include <QMessageBox>
#include "common/app_config.h"
#include "common/printer_profile.h"
#include "billing/billing_service.h"
#include "ui/billing_batch_model.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    // Test 1: AppConfig defaults
    QMessageBox::information(nullptr, "Test", "Starting tests...");
    
    try {
        auto cfg = wpce::common::AppConfig::defaults();
        QMessageBox::information(nullptr, "Test", "AppConfig::defaults() OK");
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Error", QString("AppConfig::defaults() failed: %1").arg(e.what()));
        return 1;
    }
    
    try {
        auto printers = wpce::common::detect_printer_capabilities();
        QMessageBox::information(nullptr, "Test", QString("detect_printer_capabilities() OK, found %1 printers").arg(printers.profiles.size()));
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Error", QString("detect_printer_capabilities() failed: %1").arg(e.what()));
        return 1;
    }
    
    try {
        auto* service = new wpce::ui::BillingService();
        QMessageBox::information(nullptr, "Test", "BillingService created OK");
        delete service;
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Error", QString("BillingService failed: %1").arg(e.what()));
        return 1;
    }
    
    QMessageBox::information(nullptr, "Test", "All tests passed!");
    return 0;
}