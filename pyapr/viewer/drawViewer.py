from pyqtgraph.Qt import QtCore, QtGui, QtWidgets
import numpy as np
import pyqtgraph as pg
import sys
import pyapr
import matplotlib.pyplot as plt


class customDrawMode():
    def __call__(self, dk, image, mask, ss, ts, ev):
        # print(ts)
        tst = (ts[0], ts[1])
        sst = (ss[0], ss[1])
        src = dk[sst]
        if mask is not None:
            mask = mask[sst]
            image[tst] = image[tst] * (1 - mask) + src * mask
        else:
            image[tst] = src


class MainWindow(QtGui.QWidget):

    def __init__(self):
        super(MainWindow, self).__init__()

        QtGui.QWidget.setMouseTracking(self, True)

        self.layout = QtGui.QGridLayout()
        self.layout.setSpacing(0)
        self.setLayout(self.layout)

        self.pg_win = pg.GraphicsView()
        self.view = pg.ViewBox()
        self.view.setAspectLocked()
        self.pg_win.setCentralItem(self.view)
        self.layout.addWidget(self.pg_win, 0, 0, 3, 1)

        # add a slider
        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal, self)

        self.slider.valueChanged.connect(self.valuechange)

        self.setGeometry(300, 300, self.full_size, self.full_size)

        self.layout.addWidget(self.slider, 1, 0)

        # add a histogram

        self.hist = pg.HistogramLUTWidget()

        self.layout.addWidget(self.hist, 0, 1)

        self.hist.item.sigLevelsChanged.connect(self.histogram_updated)

        self.drawing = False
        self.lastPoint = 0

        # add a drop box for LUT selection

        self.comboBox = QtGui.QComboBox(self)
        self.comboBox.move(20, 20)
        self.comboBox.addItem('bone')
        self.comboBox.addItem('viridis')
        self.comboBox.addItem('plasma')
        self.comboBox.addItem('inferno')
        self.comboBox.addItem('magma')
        self.comboBox.addItem('cividis')
        self.comboBox.addItem('Greys')
        self.comboBox.addItem('Greens')
        self.comboBox.addItem('Oranges')
        self.comboBox.addItem('Reds')
        self.comboBox.addItem('Pastel1')

        self.comboBox.currentTextChanged.connect(self.updatedLUT)

        # add a QLabel giving information on the current slice and the APR
        self.slice_info = QtGui.QLabel(self)

        self.slice_info.move(130, 20)
        self.slice_info.setFixedWidth(200)

        # add a label for the current cursor position

        self.cursor = QtGui.QLabel(self)

        self.cursor.move(330, 20)
        self.cursor.setFixedWidth(200)
        self.cursor.setFixedHeight(45)

    def add_foreground_toggle(self):
        self.draw_fg_toggle = QtWidgets.QCheckBox(self)
        self.draw_fg_toggle.setText("Draw Foreground")
        self.draw_fg_toggle.resize(140, 20)
        self.draw_fg_toggle.move(580, 20)
        self.draw_fg_toggle.setChecked(False)
        self.draw_fg_toggle.stateChanged.connect(self.toggleForeground)

    def add_background_toggle(self):
        self.draw_bg_toggle = QtWidgets.QCheckBox(self)
        self.draw_bg_toggle.setText("Draw Background")
        self.draw_bg_toggle.resize(140, 20)
        self.draw_bg_toggle.move(580, 40)
        self.draw_bg_toggle.setChecked(False)
        self.draw_bg_toggle.stateChanged.connect(self.toggleBackground)

    def toggleForeground(self):
        if self.draw_fg_toggle.isChecked():
            self.draw_bg_toggle.setChecked(False)

            self.view.setMouseEnabled(False, False)  # Disable mouse controls of viewbox when drawing
            self.view.addItem(self.fg_canvas)
        else:
            self.view.setMouseEnabled(True, True)
            self.view.removeItem(self.fg_canvas)

    def toggleBackground(self):
        if self.draw_bg_toggle.isChecked():
            self.draw_fg_toggle.setChecked(False)

            self.view.setMouseEnabled(False, False)
        else:
            self.view.setMouseEnabled(True, True)

    img_list = []
    img_list_fg = []
    img_list_bg = []

    current_view = 0

    array_int = np.array(1)
    aAPR_ref = 0
    parts_ref = 0
    fg_parts_ref = 0
    bg_parts_ref = 0

    dtype = 0

    x_num = 0
    z_num = 0
    y_num = 0

    array_list = []
    array_list_fg = []
    array_list_bg = []
    fg_arr = None
    fg_canvas = None
    fg_pos_list = []

    level_max = 0
    level_min = 0

    full_size = 900
    scale_sc = 10

    min_x = 0
    min_y = 0

    hist_min = 0
    hist_max = 1

    lut = 0
    lut_back = 0

    hist_on = True

    def updateSliceText(self, slice):

        text_string = 'Slice: ' + str(slice) + '/' + str(self.z_num) + ", " + str(self.y_num) + 'x' + str(self.x_num) + '\n'
        text_string += 'level_min: ' + str(self.level_min) + ', level_max: ' + str(self.level_max) + '\n'

        self.slice_info.setText(text_string)

    def updatedLUT(self):
        # monitors the event of the drop box being manipulated
        self.setLUT(self.comboBox.currentText())

    def setLUT(self, string):

        call_dict = {
            'viridis': plt.cm.viridis,
            'plasma': plt.cm.plasma,
            'inferno': plt.cm.inferno,
            'magma': plt.cm.magma,
            'cividis': plt.cm.cividis,
            'Greys': plt.cm.Greys,
            'Greens': plt.cm.Greens,
            'Oranges': plt.cm.Oranges,
            'Reds': plt.cm.Reds,
            'bone': plt.cm.bone,
            'Pastel1': plt.cm.Pastel1
        }

        # color map integration using LUT
        self.cmap = call_dict[string]

        self.lut = self.cmap(np.linspace(0.0, 1.0, 512))
        self.lut = self.lut * 255

        self.lut_back = self.lut.copy()
        self.lut[0, :] = 0
        self.lut_back[0, 3] = 255

        self.lut[1, :] = 0

        self.zero_img.setLookupTable(self.lut_back, True)

        for l in range(self.level_min, self.level_max + 1):
            self.img_list[l].setLookupTable(self.lut, True)

        self.green = call_dict['Greens']
        self.lut_fg = self.green(np.linspace(0.0, 1.0, 512)) * 255
        self.lut_fg[0, :] = 0
        self.lut_fg[1, :] = 255
        self.fg_canvas.setLookupTable(self.lut_fg, True)

    def init_APR(self, aAPR, parts, fg_parts, bg_parts):
        self.aAPR_ref = aAPR
        self.parts_ref = parts
        self.fg_parts_ref = fg_parts
        self.bg_parts_ref = bg_parts

        if isinstance(parts, pyapr.FloatParticles):
            self.dtype = np.float32
        elif isinstance(parts, pyapr.ShortParticles):
            self.dtype = np.uint16
        else:
            raise Exception("APR viewer is currently only implemented for particles of type Float or Short")

        self.z_num = aAPR.z_num(aAPR.level_max())
        self.x_num = aAPR.x_num(aAPR.level_max())
        self.y_num = aAPR.y_num(aAPR.level_max())
        self.level_max = aAPR.level_max()
        self.level_min = pyapr.viewer.min_occupied_level(self.aAPR_ref)

        ## Set up the slide
        self.slider.setMinimum(0)
        self.slider.setMaximum(self.z_num-1)
        self.slider.setTickPosition(QtWidgets.QSlider.TicksBothSides)
        self.slider.setGeometry(0.05*self.full_size, 0.97*self.full_size, 0.95*self.full_size, 40)

        ## Viewer elements
        self.setWindowTitle('Demo APR Viewer')

        self.view.setAspectLocked(True)

        self.view.setRange(QtCore.QRectF(0, 0, self.full_size, self.full_size))

        for i in range(0, self.level_max + 1):
            xl = aAPR.x_num(i)
            yl = aAPR.y_num(i)

            self.array_list.append(np.zeros([xl, yl], dtype=self.dtype))
            self.img_list.append(pg.ImageItem())

            # self.array_list_fg.append(np.zeros([xl, yl], dtype=np.uint16))
            # self.img_list_fg.append(pg.ImageItem())
            #
            # self.array_list_bg.append(np.zeros([xl, yl], dtype=np.uint16))
            # self.img_list_bg.append(pg.ImageItem())

        #
        #   Init the images
        #

        max_x = 0
        max_y = 0

        for l in range(self.level_min, self.level_max + 1):
            sz = pow(2, self.level_max - l)
            img_sz_x = self.array_list[l].shape[1] * sz
            img_sz_y = self.array_list[l].shape[0] * sz
            max_x = max(max_x, img_sz_x)
            max_y = max(max_y, img_sz_y)

        #
        #   Setting the scale of the image to initialize
        #
        max_dim = max(max_x, max_y)
        self.scale_sc = self.full_size/max_dim

        max_x = max_x*self.scale_sc
        max_y = max_y*self.scale_sc

        self.zero_array = np.zeros([1, 1], dtype=self.dtype)
        self.zero_array[0, 0] = 0
        self.zero_img = pg.ImageItem(self.zero_array)
        self.view.addItem(self.zero_img)
        self.zero_img.setRect(QtCore.QRectF(self.min_x, self.min_y, max_x, max_y))

        for l in range(self.level_min, self.level_max + 1):
            self.view.addItem(self.img_list[l])

        self.fg_arr = np.zeros((self.x_num, self.y_num))
        self.fg_canvas = pg.ImageItem(self.fg_arr)
        self.fg_canvas.setImage(self.fg_arr, levels=(254, 255), opacity=0.8)
        kern = np.array([[255]])
        self.fg_canvas.setDrawKernel(kern, mask=kern, center=(0, 0), mode='set')
        self.fg_canvas.drawMode = customDrawMode()
        self.fg_canvas.setRect(QtCore.QRectF(self.min_x, self.min_y,
                                             self.scale_sc * self.fg_arr.shape[1],
                                             self.scale_sc * self.fg_arr.shape[0]))

        self.setLUT('bone')

        self.current_view = -pow(2, self.level_max-self.level_min+1)
        self.update_slice(int(self.z_num*0.5))

        ## Setting up the histogram

        ## Needs to be updated to relay on a subsection of the particles
        arr = np.array(parts, copy=False)
        arr = arr.reshape((arr.shape[0], 1))

        ## then need to make it 2D, so it can be interpreted as an img;

        self.img_hist = pg.ImageItem(arr)
        self.hist.setImageItem(self.img_hist)

        ## Image hover event
        self.img_list[self.level_max].hoverEvent = self.imageHoverEvent

    def update_slice(self, new_view):

        if (new_view >= 0) & (new_view < self.z_num):
            # now update the view
            for l in range(self.level_min, self.level_max + 1):
                # loop over levels of the APR
                sz = pow(2, self.level_max - l)

                curr_z = int(new_view/sz)
                prev_z = int(self.current_view/sz)

                if prev_z != curr_z:

                    if self.draw_fg_toggle.isChecked():
                        pyapr.viewer.fill_slice_level(self.aAPR_ref, self.parts_ref, self.array_list[l], curr_z, l)
                    else:
                        pyapr.viewer.fill_slice(self.aAPR_ref, self.parts_ref, self.array_list[l], curr_z, l)

                    self.img_list[l].setImage(self.array_list[l], False)

                    img_sz_x = self.scale_sc * self.array_list[l].shape[1] * sz
                    img_sz_y = self.scale_sc * self.array_list[l].shape[0] * sz

                    self.img_list[l].setRect(QtCore.QRectF(self.min_x, self.min_y, img_sz_x, img_sz_y))

            # fg_pos = np.transpose(np.nonzero(self.fg_arr))
            # self.fg_arr.fill(0)

            prev_z = self.current_view
            self.current_view = new_view
            # make the slider reflect the new value
            self.slider.setValue(new_view)
            self.updateSliceText(new_view)

            # if fg_pos.size > 0:
            #     print(fg_pos)

    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            self.drawing = True
            self.lastPoint = event.pos()
            print(self.lastPoint)
            # i, j = self.lastPoint.y(), self.lastPoint.x()
            # self.fg_arr[i, j] = 255
            # print(self.lastPoint)

    def mouseMoveEvent(self, event):
        if self.drawing:
            self.lastPoint = event.pos()
            if self.draw_fg_toggle.isChecked():
                self.fg_pos_list.append((self.lastPoint.y(), self.lastPoint.x()))


    # def imgDrawEvent(self, event):
    #     if (event.buttons() & QtCore.Qt.LeftButton) & self.drawing:
    #         if self.draw_fg_toggle.isChecked():
    #
    #             self.lastPoint = event.pos()
    #             self.fg_pos_list.append((self.lastPoint.y(), self.lastPoint.x()))
    #             print(self.lastPoint)
    #             # self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            if self.draw_fg_toggle.isChecked():
                print('mouse released')
                print('fg positions: {}'.format(len(self.fg_pos_list)))
                self.fg_pos_list = []

            self.drawing = False

    def keyPressEvent(self, event):
        if event.key() == QtCore.Qt.Key_Left:
            # back a frame
            self.update_slice(self.current_view - 1)

        if event.key() == QtCore.Qt.Key_Right:
            # forward a frame
            self.update_slice(self.current_view + 1)

    def valuechange(self):
        size = self.slider.value()
        self.update_slice(size)

    def histogram_updated(self):

        if self.hist_on:
            hist_range = self.hist.item.getLevels()

            self.hist_min = hist_range[0]
            self.hist_max = hist_range[1]

            for l in range(self.level_min, self.level_max + 1):
                self.img_list[l].setLevels([self.hist_min,  self.hist_max], True)

                self.zero_img.setLevels([0,  1], True)


    def imageHoverEvent(self, event):
        """
        Show the position, pixel, and value under the mouse cursor.
        """

        if event.isExit():
            return

        current_level = self.level_max

        data = self.array_list[self.level_max]

        pos = event.pos()
        i, j = pos.y(), pos.x()
        i = int(np.clip(i, 0, data.shape[0] - 1))
        j = int(np.clip(j, 0, data.shape[1] - 1))
        val = data[i, j]

        i_l = i
        j_l = j

        while (val == 0) & (current_level > self.level_min):
            current_level -= 1
            i_l = int(i_l/2)
            j_l = int(j_l/2)
            val = self.array_list[current_level][i_l, j_l]

        text_string = "(y: " + str(i) + ",x: " + str(j) + ") val: " + str(val) + ")" + "\n"
        text_string += "(y_l: " + str(i_l) + ",x_l: " + str(j_l) + ",l: " + str(current_level) + ")" + "\n"

        self.cursor.setText(text_string)


def draw_viewer(aAPR, parts):
    pg.setConfigOption('background', 'w')
    pg.setConfigOption('foreground', 'k')

    app = QtGui.QApplication.instance()
    if app is None:
        app = QtGui.QApplication([])

    pg.setConfigOption('imageAxisOrder', 'row-major')

    foregroundParts = pyapr.ShortParticles(aAPR)
    backgroundParts = pyapr.ShortParticles(aAPR)

    ## Create window with GraphicsView widget
    win = MainWindow()

    win.add_foreground_toggle()
    win.add_background_toggle()

    win.init_APR(aAPR, parts, foregroundParts, backgroundParts)

    win.show()

    app.exec_()

    print(len(np.nonzero(win.fg_arr)))

    return foregroundParts, backgroundParts