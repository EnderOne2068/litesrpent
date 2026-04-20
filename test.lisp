;; This is designed to test litesrpent
(defparameter *input* "")
(format t "Say 'I am done testing.' to leave."
(loop do
(format t "~%~%Test things here: ")
(finish-output)
(setf *input* (read-line))
(cond
((string= *input* "~%I am done testing."
(return)))))
