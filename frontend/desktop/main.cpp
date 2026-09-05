#include <gtk/gtk.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

struct FrontendState {
    GtkTextBuffer* output{};
    GtkLabel* status{};
    GtkEntry* command_entry{};
    std::string cli;
    bool busy{};
};

struct CommandContext {
    FrontendState* state{};
    GSubprocess* process{};
};

[[nodiscard]] std::filesystem::path executable_path(const char* argv0) {
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<std::size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data());
    }
    return std::filesystem::absolute(argv0 == nullptr ? "neuroshade-desktop" : argv0);
}

[[nodiscard]] std::string cli_path(const char* argv0) {
    return (executable_path(argv0).parent_path() / "neuroshade").string();
}

void set_output(FrontendState& state, const char* text) {
    gtk_text_buffer_set_text(state.output, text == nullptr ? "" : text, -1);
}

void command_finished(GObject*, GAsyncResult* result, gpointer user_data) {
    auto* context = static_cast<CommandContext*>(user_data);
    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    GError* error = nullptr;
    const gboolean communicated = g_subprocess_communicate_utf8_finish(
        context->process, result, &stdout_text, &stderr_text, &error);

    std::string message;
    if (communicated) {
        if (stdout_text != nullptr) message += stdout_text;
        if (stderr_text != nullptr) message += stderr_text;
        if (message.empty()) message = "Comando concluído sem saída.\n";
    } else {
        message = "Falha ao executar o comando: ";
        message += error == nullptr ? "erro desconhecido" : error->message;
        message += '\n';
    }
    set_output(*context->state, message.c_str());
    const bool success = communicated && g_subprocess_get_successful(context->process);
    gtk_label_set_text(context->state->status,
                       success ? "Concluído" : "Concluído com diagnóstico");
    context->state->busy = false;

    g_clear_error(&error);
    g_free(stdout_text);
    g_free(stderr_text);
    g_object_unref(context->process);
    delete context;
}

void run_cli(FrontendState& state, std::string_view arguments) {
    if (state.busy) {
        gtk_label_set_text(state.status, "Aguarde o comando atual terminar");
        return;
    }

    const std::string command(arguments);
    gint parsed_count = 0;
    gchar** parsed_values = nullptr;
    GError* error = nullptr;
    if (!g_shell_parse_argv(command.c_str(), &parsed_count, &parsed_values, &error)) {
        std::string message = "Argumentos inválidos: ";
        message += error == nullptr ? "erro desconhecido" : error->message;
        set_output(state, message.c_str());
        gtk_label_set_text(state.status, "Comando inválido");
        g_clear_error(&error);
        return;
    }

    std::vector<const gchar*> argv;
    argv.reserve(static_cast<std::size_t>(parsed_count) + 2);
    argv.push_back(state.cli.c_str());
    for (gint index = 0; index < parsed_count; ++index) argv.push_back(parsed_values[index]);
    argv.push_back(nullptr);

    GSubprocess* process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_MERGE),
        &error);
    g_strfreev(parsed_values);
    if (process == nullptr) {
        std::string message = "Não foi possível iniciar o CLI: ";
        message += error == nullptr ? "erro desconhecido" : error->message;
        set_output(state, message.c_str());
        gtk_label_set_text(state.status, "Falha ao iniciar");
        g_clear_error(&error);
        return;
    }

    state.busy = true;
    gtk_label_set_text(state.status, "Executando…");
    auto* context = new CommandContext{&state, process};
    g_subprocess_communicate_utf8_async(process, nullptr, nullptr, command_finished, context);
}

void command_entry_activated(GtkEntry* entry, gpointer user_data) {
    auto* state = static_cast<FrontendState*>(user_data);
    const char* command = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (command != nullptr && command[0] != '\0') run_cli(*state, command);
}

void action_clicked(GtkButton* button, gpointer user_data) {
    auto* state = static_cast<FrontendState*>(user_data);
    const auto* command = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "ns-command"));
    if (command != nullptr) run_cli(*state, command);
}

GtkWidget* action_button(const char* label, const char* command, FrontendState* state) {
    GtkWidget* button = gtk_button_new_with_label(label);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    g_object_set_data(G_OBJECT(button), "ns-command", const_cast<char*>(command));
    g_signal_connect(button, "clicked", G_CALLBACK(action_clicked), state);
    return button;
}

void activate(GtkApplication* application, gpointer user_data) {
    auto* state = static_cast<FrontendState*>(user_data);
    GtkWidget* window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "NeuroShade");
    gtk_window_set_default_size(GTK_WINDOW(window), 920, 620);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_window_set_child(GTK_WINDOW(window), root);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title),
                         "<span size='x-large' weight='bold'>NeuroShade Control Center</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), title);

    GtkWidget* subtitle = gtk_label_new(
        "Configuração e diagnóstico fora do processo do jogo. Nenhum renderizador é injetado por esta janela.");
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), subtitle);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(root), paned);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(actions, 210, -1);
    gtk_paned_set_start_child(GTK_PANED(paned), actions);
    gtk_box_append(GTK_BOX(actions), action_button("Diagnóstico", "doctor", state));
    gtk_box_append(GTK_BOX(actions), action_button("Autoteste completo", "doctor --deep", state));
    gtk_box_append(GTK_BOX(actions), action_button("Jogos", "game list", state));
    gtk_box_append(GTK_BOX(actions), action_button("Plugins", "plugin list", state));
    gtk_box_append(GTK_BOX(actions), action_button("Modelos", "model list", state));
    gtk_box_append(GTK_BOX(actions), action_button("Verificar pacotes", "verify", state));
    gtk_box_append(GTK_BOX(actions), action_button("Diretório de logs", "logs", state));

    GtkWidget* advanced_label = gtk_label_new("Comando avançado");
    gtk_widget_set_halign(advanced_label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(advanced_label, 12);
    gtk_box_append(GTK_BOX(actions), advanced_label);
    GtkWidget* command_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(command_entry),
                                   "Ex.: plugin install '/caminho' ");
    gtk_widget_set_tooltip_text(
        command_entry,
        "Executa argumentos do CLI sem shell; use aspas em caminhos que contenham espaços.");
    state->command_entry = GTK_ENTRY(command_entry);
    g_signal_connect(command_entry, "activate", G_CALLBACK(command_entry_activated), state);
    gtk_box_append(GTK_BOX(actions), command_entry);
    GtkWidget* run_button = gtk_button_new_with_label("Executar comando CLI");
    g_signal_connect_swapped(run_button, "clicked", G_CALLBACK(gtk_widget_activate), command_entry);
    gtk_box_append(GTK_BOX(actions), run_button);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_paned_set_end_child(GTK_PANED(paned), scroller);

    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    state->output = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    set_output(*state,
               "Selecione uma ação. Para instalar ou importar pacotes, use o CLI `neuroshade`; "
               "a interface nunca executa código no processo do jogo.\n");

    GtkWidget* status = gtk_label_new("Pronto — camada Vulkan desativada até o lançamento explícito");
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    state->status = GTK_LABEL(status);
    gtk_box_append(GTK_BOX(root), status);

    gtk_window_present(GTK_WINDOW(window));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cli = cli_path(argc > 0 ? argv[0] : nullptr);
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        if (access(cli.c_str(), X_OK) != 0) {
            std::cerr << "desktop_frontend=self-test-fail reason=cli-not-executable path="
                      << cli << '\n';
            return 1;
        }
        std::cout << "desktop_frontend=self-test-pass render_process=no toolkit=gtk4\n";
        return 0;
    }

    FrontendState state{};
    state.cli = cli;
    GtkApplication* application = gtk_application_new(
        "org.neuroshade.NeuroShade", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    const int result = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return result;
}
