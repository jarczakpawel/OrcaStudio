#include "ModelMall.hpp"
#include "GUI_App.hpp"

#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/uri.h>
#include "wx/evtloop.h"

#include "libslic3r/Model.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.hpp"

namespace Slic3r {
namespace GUI {
    ModelMallDialog::ModelMallDialog(Plater* plater /*= nullptr*/)
        :DPIFrame(nullptr, wxID_ANY, _L("3D Models"), wxDefaultPosition, wxDefaultSize, wxCLOSE_BOX|wxDEFAULT_DIALOG_STYLE|wxMAXIMIZE_BOX|wxMINIMIZE_BOX|wxRESIZE_BORDER),
         m_linux_browser_timer(this)
    {
        SetSize(MODEL_MALL_PAGE_SIZE);
        SetMinSize(wxSize(MODEL_MALL_PAGE_SIZE.x / 4, MODEL_MALL_PAGE_SIZE.y / 4));

        wxBoxSizer* m_sizer_main = new wxBoxSizer(wxVERTICAL);

        auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
        m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
        m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);

        m_web_control_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, MODEL_MALL_PAGE_CONTROL_SIZE, wxTAB_TRAVERSAL);
        m_web_control_panel->SetBackgroundColour(*wxWHITE);
        m_web_control_panel->SetSize(MODEL_MALL_PAGE_CONTROL_SIZE);


        wxBoxSizer* m_sizer_web_control = new wxBoxSizer(wxHORIZONTAL);

        auto m_control_back = new ScalableButton(m_web_control_panel, wxID_ANY, "mall_control_back", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_control_back->SetBackgroundColour(*wxWHITE);
        m_control_back->SetSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_back->SetMinSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_back->SetMaxSize(wxSize(FromDIP(25), FromDIP(30)));

        m_control_back->Bind(wxEVT_LEFT_DOWN, &ModelMallDialog::on_back, this);
        m_control_back->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_HAND));});
        m_control_back->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_ARROW));});


        auto m_control_forward = new ScalableButton(m_web_control_panel, wxID_ANY, "mall_control_forward", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_control_forward->SetBackgroundColour(*wxWHITE);
        m_control_forward->SetSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_forward->SetMinSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_forward->SetMaxSize(wxSize(FromDIP(25), FromDIP(30)));

        m_control_forward->Bind(wxEVT_LEFT_DOWN, &ModelMallDialog::on_forward, this);
        m_control_forward->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_HAND)); });
        m_control_forward->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_ARROW)); });

        auto m_control_refresh = new ScalableButton(m_web_control_panel, wxID_ANY, "mall_control_refresh", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_control_refresh->SetBackgroundColour(*wxWHITE);
        m_control_refresh->SetSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_refresh->SetMinSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_refresh->SetMaxSize(wxSize(FromDIP(25), FromDIP(30)));
        m_control_refresh->Bind(wxEVT_LEFT_DOWN, &ModelMallDialog::on_refresh, this);
        m_control_refresh->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_HAND)); });
        m_control_refresh->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) {SetCursor(wxCursor(wxCURSOR_ARROW)); });

#ifdef __APPLE__
        // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
        m_control_back->SetToolTip(_L("Click to return") + "(" + u8"\u2318+" /* u8"⌘+" */ + _L("Left Arrow") + ")");
        m_control_forward->SetToolTip(_L("Click to continue") + "(" + u8"\u2318+"  /* u8"⌘+" */ + _L("Right Arrow") + ")");
#else
        // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
        m_control_back->SetToolTip(_L("Click to return") + "(" + _L("Alt+") + _L("Left Arrow") + ")");
        m_control_forward->SetToolTip(_L("Click to continue") + "(" + _L("Alt+") + _L("Right Arrow") + ")");
#endif

        m_control_refresh->SetToolTip(_L("Refresh"));
        /* auto m_textCtrl1 = new wxTextCtrl(m_web_control_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(600, 30), 0);
         auto m_button1 = new wxButton(m_web_control_panel, wxID_ANY, wxT("GO"), wxDefaultPosition, wxDefaultSize, 0);
         m_button1->Bind(wxEVT_BUTTON, [this,m_textCtrl1](auto& e) {
             go_to_url(m_textCtrl1->GetValue());
         });*/

        m_sizer_web_control->Add( m_control_back, 0, wxALIGN_CENTER | wxLEFT, FromDIP(26) );
        m_sizer_web_control->Add(m_control_forward, 0, wxALIGN_CENTER | wxLEFT, FromDIP(26));
        m_sizer_web_control->Add(m_control_refresh, 0, wxALIGN_CENTER | wxLEFT, FromDIP(26));
        //m_sizer_web_control->Add(m_button1, 0, wxALIGN_CENTER|wxLEFT, 5);
        //m_sizer_web_control->Add(m_textCtrl1, 0, wxALIGN_CENTER|wxLEFT, 5);

        m_web_control_panel->SetSizer(m_sizer_web_control);
        m_web_control_panel->Layout();
        m_sizer_web_control->Fit(m_web_control_panel);

        m_browser = WebView::CreateWebView(this, wxEmptyString);
        if (m_browser == nullptr) {
            wxLogError("Could not init m_browser");
            return;
        }

        m_browser->SetSize(MODEL_MALL_PAGE_WEB_SIZE);
        m_browser->SetMinSize(MODEL_MALL_PAGE_WEB_SIZE);
        m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ModelMallDialog::OnScriptMessage, this, m_browser->GetId());
        m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &ModelMallDialog::on_linux_viewer_navigation, this, m_browser->GetId());
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &ModelMallDialog::on_linux_viewer_new_window, this, m_browser->GetId());

        m_sizer_main->Add(m_web_control_panel, 0, wxEXPAND, 0);
        m_sizer_main->Add(m_browser, 1, wxEXPAND, 0);
        SetSizer(m_sizer_main);
        Layout();
        Fit();

        Centre(wxBOTH);
        Bind(wxEVT_SHOW, &ModelMallDialog::on_show, this);
        Bind(wxEVT_TIMER, &ModelMallDialog::on_linux_browser_timer, this, m_linux_browser_timer.GetId());

        Bind(wxEVT_CLOSE_WINDOW, [this](auto& e) {
            stop_linux_browser();
            this->Hide();
        });
    }


    ModelMallDialog::~ModelMallDialog()
    {
        stop_linux_browser();
    }

    void ModelMallDialog::OnScriptMessage(wxWebViewEvent& evt)
    {
        try {
            wxString strInput = evt.GetString();
            json     j = json::parse(strInput.utf8_string());

            wxString strCmd = j["command"];

            if(strCmd == "request_close_publish_window") {
                this->Hide();
            }

        }
        catch (std::exception&) {
            // wxMessageBox(e.what(), "json Exception", MB_OK);
        }
    }

    void ModelMallDialog::on_dpi_changed(const wxRect& suggested_rect)
    {
    }

    void ModelMallDialog::on_show(wxShowEvent& event)
    {
        wxGetApp().UpdateFrameDarkUI(this);
        if (event.IsShown()) {
            Centre(wxBOTH);
        }
        /*else {
            go_to_url(m_url);
        }*/
        event.Skip();
    }

    void ModelMallDialog::on_refresh(wxMouseEvent& evt)
    {
        if (m_linux_browser_active) {
            send_linux_browser_command({{"command", "reload"}});
        } else if (!m_browser->GetCurrentURL().empty()) {
            m_browser->Reload();
        }
    }

    void ModelMallDialog::on_back(wxMouseEvent& evt)
    {
        if (m_linux_browser_active) {
            send_linux_browser_command({{"command", "back"}});
        } else if (m_browser->CanGoBack()) {
            m_browser->GoBack();
        }
    }

    void ModelMallDialog::on_forward(wxMouseEvent& evt)
    {
        if (m_linux_browser_active) {
            send_linux_browser_command({{"command", "forward"}});
        } else if (m_browser->CanGoForward()) {
            m_browser->GoForward();
        }
    }

    void ModelMallDialog::go_to_url(wxString url, bool bind_ticket)
    {
        if (Slic3r::SlicerLinuxRuntime::use_linux_runtime()) {
            const std::string target = into_u8(url);
            if (m_linux_browser_active) {
                if (!send_linux_browser_command({{"command", "load_url"}, {"url", target}}))
                    stop_linux_browser();
                else
                    return;
            }

            std::string start_reply;
            bool browser_available = false;
            {
                auto module_lock = BBLNetworkPlugin::lock_module_for_call();
                auto& plugin = BBLNetworkPlugin::instance();
                auto start = plugin.get_linux_browser_start();
                auto status = plugin.get_linux_browser_status();
                auto command = plugin.get_linux_browser_command();
                auto cancel = plugin.get_linux_browser_cancel();
                browser_available = start && status && command && cancel;
                if (browser_available)
                    start_reply = start(plugin.get_agent(), target, bind_ticket);
            }
            if (!browser_available) {
                show_error(this, _L("Linux Bambu browser is not available in the installed runtime."));
                return;
            }
            try {
                const json reply = json::parse(start_reply);
                if (!reply.value("ok", false)) {
                    show_error(this, from_u8(reply.value("error", std::string("Linux Bambu browser failed to start"))));
                    return;
                }
                const std::string viewer_url = reply.value("viewer_url", std::string());
                if (viewer_url.empty()) {
                    show_error(this, _L("Linux Bambu browser returned no viewer URL."));
                    return;
                }
                const wxURI viewer_uri(from_u8(viewer_url));
                long viewer_port = 0;
                if (!viewer_uri.GetPort().ToLong(&viewer_port) || viewer_port <= 0 || viewer_port > 65535) {
                    show_error(this, _L("Linux Bambu browser returned an invalid viewer URL."));
                    return;
                }
                m_linux_viewer_port = static_cast<int>(viewer_port);
                m_linux_browser_active = true;
                m_linux_browser_timer.Start(250);
                m_browser->LoadURL(from_u8(viewer_url));
                return;
            } catch (const std::exception& e) {
                show_error(this, from_u8(std::string("Linux Bambu browser error: ") + e.what()));
                return;
            }
        }
        WebView::LoadUrl(m_browser, url);
    }

    bool ModelMallDialog::send_linux_browser_command(const json& command)
    {
        std::string raw_reply;
        {
            auto module_lock = BBLNetworkPlugin::lock_module_for_call();
            auto fn = BBLNetworkPlugin::instance().get_linux_browser_command();
            if (!fn)
                return false;
            raw_reply = fn(command.dump());
        }
        try {
            const json reply = json::parse(raw_reply);
            return reply.value("ok", false);
        } catch (...) {
            return false;
        }
    }

    void ModelMallDialog::stop_linux_browser()
    {
        m_linux_browser_timer.Stop();
        if (!m_linux_browser_active)
            return;
        m_linux_browser_active = false;
        m_linux_viewer_port = 0;
        auto module_lock = BBLNetworkPlugin::lock_module_for_call();
        if (auto cancel = BBLNetworkPlugin::instance().get_linux_browser_cancel())
            (void) cancel();
    }

    void ModelMallDialog::on_linux_browser_timer(wxTimerEvent&)
    {
        if (!m_linux_browser_active)
            return;
        std::string raw_status;
        bool status_available = false;
        {
            auto module_lock = BBLNetworkPlugin::lock_module_for_call();
            auto status_fn = BBLNetworkPlugin::instance().get_linux_browser_status();
            status_available = status_fn != nullptr;
            if (status_available)
                raw_status = status_fn();
        }
        if (!status_available) {
            stop_linux_browser();
            return;
        }
        try {
            const json status = json::parse(raw_status);
            if (status.contains("events") && status["events"].is_array()) {
                for (const auto& event : status["events"]) {
                    if (event.value("kind", std::string()) != "script_message" || !event.contains("message"))
                        continue;
                    const auto& message = event["message"];
                    if (message.is_object() && message.value("command", std::string()) == "request_close_publish_window") {
                        stop_linux_browser();
                        Hide();
                        return;
                    }
                }
            }
            const std::string state = status.value("state", std::string("error"));
            if (state != "running") {
                m_linux_browser_timer.Stop();
                m_linux_browser_active = false;
                m_linux_viewer_port = 0;
                if (state == "error")
                    show_error(this, from_u8(status.value("error", std::string("Linux Bambu browser stopped unexpectedly"))));
            }
        } catch (const std::exception& e) {
            stop_linux_browser();
            show_error(this, from_u8(std::string("Linux Bambu browser status error: ") + e.what()));
        }
    }


    void ModelMallDialog::on_linux_viewer_navigation(wxWebViewEvent& evt)
    {
        if (!m_linux_browser_active)
            return;
        const wxString url = evt.GetURL().Lower();
        const wxString loopback_v4 = wxString::Format("http://127.0.0.1:%d/", m_linux_viewer_port);
        const wxString loopback_name = wxString::Format("http://localhost:%d/", m_linux_viewer_port);
        if (m_linux_viewer_port <= 0 || !(url.StartsWith(loopback_v4) || url.StartsWith(loopback_name) || url.StartsWith("about:blank")))
            evt.Veto();
    }

    void ModelMallDialog::on_linux_viewer_new_window(wxWebViewEvent& evt)
    {
        if (!m_linux_browser_active)
            return;
        const wxString url = evt.GetURL().Lower();
        const wxString loopback_v4 = wxString::Format("http://127.0.0.1:%d/", m_linux_viewer_port);
        const wxString loopback_name = wxString::Format("http://localhost:%d/", m_linux_viewer_port);
        if (m_linux_viewer_port <= 0 || !(url.StartsWith(loopback_v4) || url.StartsWith(loopback_name) || url.StartsWith("about:blank"))) {
            evt.Veto();
            return;
        }
        evt.Veto();
        m_browser->LoadURL(evt.GetURL());
    }

    void ModelMallDialog::show_control(bool show)
    {
        m_web_control_panel->Show(show);
        Layout();
        Fit();
    }

    void ModelMallDialog::go_to_mall(wxString url)
    {
        /*if (!url.empty() && m_homepage_url.empty()) {
            m_homepage_url = url;
        }*/
        if(url.empty())return;
        m_url = url;
        go_to_url(url);
    }

    void ModelMallDialog::go_to_publish(wxString url)
    {
        /*if (!url.empty() && m_publish_url.empty()) {
            m_publish_url = url;
        }*/
        if(url.empty())return;
        m_url = url;
        go_to_url(url);
    }

}
} // namespace Slic3r::GUI
