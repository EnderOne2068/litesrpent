;;; gl_triangle.lisp -- OpenGL triangle rendering demo
;;; Creates a window, sets up an OpenGL context, draws a colored triangle.

(format t "OpenGL Triangle Demo~%")

;; Create window
(defvar *hwnd* (gui-create-window "Litesrpent OpenGL" 100 100 640 480))
(gui-show-window *hwnd*)

;; Create GL context
(defvar *gl-ctx* (gl-create-context *hwnd*))
(format t "GL context created.~%")

;; Setup
(gl-clear-color 0.1 0.1 0.2 1.0)
(gl-viewport 0 0 640 480)
(gl-matrix-mode #x1701) ;; GL_PROJECTION
(gl-load-identity)
(gl-ortho -1.0 1.0 -1.0 1.0 -1.0 1.0)
(gl-matrix-mode #x1700) ;; GL_MODELVIEW
(gl-load-identity)

;; Draw loop (simplified: draw once, then message pump)
(defun draw-triangle ()
  (gl-clear #x4000) ;; GL_COLOR_BUFFER_BIT
  (gl-begin #x0004)  ;; GL_TRIANGLES
  (gl-color3f 1.0 0.0 0.0)  (gl-vertex2f  0.0  0.8)
  (gl-color3f 0.0 1.0 0.0)  (gl-vertex2f -0.8 -0.8)
  (gl-color3f 0.0 0.0 1.0)  (gl-vertex2f  0.8 -0.8)
  (gl-end)
  (gl-flush)
  (gl-swap-buffers *hwnd*))

(draw-triangle)
(format t "Triangle drawn. Close window to exit.~%")
(gui-pump-messages)
(gl-destroy-context *gl-ctx*)
(format t "Done.~%")
