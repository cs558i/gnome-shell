// -*- mode: js; js-indent-level: 4; indent-tabs-mode: nil -*-
//
// A widget showing a URL for web login
/* exported WebLoginPrompt */

const Clutter = imports.gi.Clutter;
const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Shell = imports.gi.Shell;
const St = imports.gi.St;

const Params = imports.misc.params;

var QR_CODE_SIZE = 128;

var QrCode = GObject.registerClass(
class QrCode extends St.Bin {
    _init(params) {
        let themeContext = St.ThemeContext.get_for_stage(global.stage);
        params = Params.parse(params, {
            styleClass: 'qr-code',
            iconSize: QR_CODE_SIZE,
            url: null
        });

        super._init({
            style_class: params.styleClass,
            width: params.iconSize * themeContext.scaleFactor,
            height: params.iconSize * themeContext.scaleFactor,
        });

        this._qrCodeGenerator = new Shell.QrCodeGenerator();
        this._iconSize = params.iconSize;
        this._url = params.url;
        this.child = new St.Icon({
            icon_size: this._iconSize,
        });

        themeContext.connectObject('notify::scale-factor', this.update.bind(this), this);

        this.update();
    }

    vfunc_style_changed() {
        super.vfunc_style_changed();

        let node = this.get_theme_node();
        let [found, iconSize] = node.lookup_length('icon-size', false);

        if (!found)
            return;

        let themeContext = St.ThemeContext.get_for_stage(global.stage);

        this._iconSize = iconSize / themeContext.scaleFactor;
        this.update();
    }

    update() {
        let { scaleFactor } = St.ThemeContext.get_for_stage(global.stage);
        this.set_size(
            this._iconSize * scaleFactor,
            this._iconSize * scaleFactor);

        this._qrCodeGenerator.generate_qr_code (this._url, this._iconSize, this._iconSize, (o, result) => {
            log("QR Code generation complete");

            let gicon = this._qrCodeGenerator.generate_qr_code_finish (result);
            this.child.gicon = gicon;
        });

        this.style = null;
    }
});

var WebLoginInfo = GObject.registerClass(
class WebLoginInfo extends St.Widget {
    _init(url, code) {
        super._init({ layout_manager: new Clutter.BoxLayout({ orientation: Clutter.Orientation.VERTICAL  },),
                       x_expand: true,});

        this._urlTitleLabel = new St.Label({
            text: _("Web Login"),
            style_class: 'web-login-title-label',
        });
        this.add_child(this._urlTitleLabel);

        this._urlLabel = new St.Label({
            style_class: 'web-login-url-label',
            text: url,
            x_expand: false,
        });
        this.add_child(this._urlLabel);

        if (code) {
            this._codeTitleLabel = new St.Label({
                text: _("Code"),
                style_class: 'web-login-title-label',
            });
            this.add_child(this._codeTitleLabel);

            this._codeLabel = new St.Label({
                text: code,
                style_class: 'web-login-code-label',
            });
            this.add_child(this._codeLabel);
        }
    }
});

var WebLoginPrompt = GObject.registerClass(
class WebLoginPrompt extends St.BoxLayout {
    _init(params) {
        params = Params.parse(params, {
            iconSize: QR_CODE_SIZE,
            url: null,
            code: null,
        });

        super._init({
            styleClass: 'web-login-prompt',
            vertical: false,
        });

        this._qrCode = new QrCode({ iconSize: params.iconSize,
                                    url: params.url });
        this.add_child(this._qrCode);

        this._webLoginInfo = new WebLoginInfo(params.url, params.code);
        this.add_child(this._webLoginInfo);
    }
});

var WebLoginIntro = GObject.registerClass(
class WebLoginIntro extends St.Label {
    _init(params) {
        params = Params.parse(params, {
            message: null,
        });

        super._init({
            styleClass: 'web-login-prompt web-login-intro-label',
            x_align: Clutter.ActorAlign.CENTER,
            x_align: Clutter.ActorAlign.CENTER,
            text: params.message
        });

        this.clutter_text.line_wrap = true;
        this.clutter_text.y_align = Clutter.ActorAlign.CENTER;
        this.clutter_text.x_align = Clutter.ActorAlign.CENTER;
    }
});
