;;; gui_hello.lisp -- Win32 GUI demonstration
;;; Creates a window with a button that shows a message box.

(format t "Creating GUI window...~%")

;; Create main window
(defvar *hwnd* (gui-create-window "Litesrpent GUI Demo" 100 100 400 300))
(gui-show-window *hwnd*)

;; Create a button
(gui-create-button *hwnd* "Click Me!" 130 100 140 40 1001)

;; Set callback
(gui-set-callback *hwnd*
  (lambda (msg wparam lparam)
    (when (and (= msg 273) (= wparam 1001))  ;; WM_COMMAND, button ID
      (gui-message-box "Hello from Litesrpent!" "Greetings" 0))))

;; Run message loop
(format t "Running message loop (close window to exit)...~%")
(gui-pump-messages)
(format t "GUI demo finished.~%")
